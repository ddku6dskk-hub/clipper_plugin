#pragma once
#include <juce_dsp/juce_dsp.h>

namespace kyohei::dsp
{
/**
 * Linked shelf フィルタペア:
 *   pre()  = RBJ shelf with +gain
 *   post() = RBJ shelf with -gain (inverse)
 *
 * サブ閾値領域では pre→post が打ち消し、完全にフラットになる。
 * クリッピングが発生すると、pre で増幅された帯域が先に天井に当たり、
 * post で元のレベルに戻されるため「帯域別クリップ」が実現される。
 *
 * Order を指定すると、gain を order 分だけ分割して直列カスケード。
 */
template <typename SampleType>
class LinkedShelf
{
public:
    enum class Kind { LowShelf, HighShelf };

    void prepare (const juce::dsp::ProcessSpec& spec, int order_ = 1)
    {
        order = juce::jmax (1, order_);
        pre.resize ((size_t) order);
        post.resize ((size_t) order);
        for (auto& f : pre)  f.prepare (spec);
        for (auto& f : post) f.prepare (spec);
        sampleRate = spec.sampleRate;
    }

    void reset()
    {
        for (auto& f : pre)  f.reset();
        for (auto& f : post) f.reset();
    }

    /** shelf パラメータを更新（non-RT-safe: heap allocation を含む）。prepare または UI 変化時のみ使用。 */
    void setShelf (Kind k, SampleType gainDb, SampleType cornerHz, SampleType slope)
    {
        kind = k;
        currentGainDb = gainDb;
        currentCornerHz = cornerHz;
        currentSlope = slope;

        const double omega = juce::MathConstants<double>::twoPi * (double) cornerHz / sampleRate;
        cosW = std::cos (omega);
        sinW = std::sin (omega);
        oneOverSMinus1 = 1.0 / (double) slope - 1.0;

        const auto gPer = gainDb / (SampleType) order;
        for (int i = 0; i < order; ++i)
        {
            auto preCoeff  = designShelf (k, +gPer, cornerHz, slope);
            auto postCoeff = designShelf (k, -gPer, cornerHz, slope);
            *pre [(size_t) i].coefficients  = *preCoeff;
            *post[(size_t) i].coefficients = *postCoeff;
        }
    }

    /**
     * RT-safe な gain 更新。corner/slope は setShelf で設定済みの値を再利用する。
     * heap allocation なし、既存の係数ストレージに直接書き込む。
     * 注意: setShelf を最低一度呼んでから使うこと（ストレージ確保のため）。
     */
    void setGainDbFast (SampleType gainDb) noexcept
    {
        currentGainDb = gainDb;
        const auto gPer = gainDb / (SampleType) order;
        for (int i = 0; i < order; ++i)
        {
            writeShelfCoeffsInPlace (pre [(size_t) i].coefficients->getRawCoefficients(), +gPer);
            writeShelfCoeffsInPlace (post[(size_t) i].coefficients->getRawCoefficients(), -gPer);
        }
    }

    /** pre-stage を適用（原音 → shelf boost/cut） */
    SampleType processPre (SampleType x) noexcept
    {
        for (auto& f : pre) x = f.processSample (x);
        return x;
    }

    /** post-stage を適用（clip 後 → 逆 shelf） */
    SampleType processPost (SampleType x) noexcept
    {
        for (auto& f : post) x = f.processSample (x);
        return x;
    }

private:
    using Coeff = juce::dsp::IIR::Coefficients<SampleType>;

    juce::ReferenceCountedObjectPtr<Coeff>
    designShelf (Kind k, SampleType gainDb, SampleType cornerHz, SampleType slope) const
    {
        // RBJ cookbook shelving filter
        const auto A = std::pow (SampleType (10), gainDb / (SampleType) 40);
        const auto omega = juce::MathConstants<SampleType>::twoPi * cornerHz / (SampleType) sampleRate;
        const auto cosLocal = std::cos (omega);
        const auto sinLocal = std::sin (omega);
        const auto val  = (A + (SampleType) 1 / A) * ((SampleType) 1 / slope - (SampleType) 1) + (SampleType) 2;
        const auto alpha = sinLocal * (SampleType) 0.5 * std::sqrt (juce::jmax ((SampleType) 0, val));
        const auto sqA = std::sqrt (A);

        SampleType b0, b1, b2, a0, a1, a2;
        if (k == Kind::HighShelf)
        {
            b0 =  A * ((A + 1) + (A - 1) * cosLocal + 2 * sqA * alpha);
            b1 = -2 * A * ((A - 1) + (A + 1) * cosLocal);
            b2 =  A * ((A + 1) + (A - 1) * cosLocal - 2 * sqA * alpha);
            a0 =       (A + 1) - (A - 1) * cosLocal + 2 * sqA * alpha;
            a1 =  2 *  ((A - 1) - (A + 1) * cosLocal);
            a2 =       (A + 1) - (A - 1) * cosLocal - 2 * sqA * alpha;
        }
        else // LowShelf
        {
            b0 =  A * ((A + 1) - (A - 1) * cosLocal + 2 * sqA * alpha);
            b1 =  2 * A * ((A - 1) - (A + 1) * cosLocal);
            b2 =  A * ((A + 1) - (A - 1) * cosLocal - 2 * sqA * alpha);
            a0 =       (A + 1) + (A - 1) * cosLocal + 2 * sqA * alpha;
            a1 = -2 *  ((A - 1) + (A + 1) * cosLocal);
            a2 =       (A + 1) + (A - 1) * cosLocal - 2 * sqA * alpha;
        }

        return new Coeff (b0 / a0, b1 / a0, b2 / a0, (SampleType) 1, a1 / a0, a2 / a0);
    }

    /**
     * in-place RBJ shelf 係数計算。cached cosW/sinW/oneOverSMinus1 を使用。
     * dst は少なくとも 5 要素を持つ配列 (b0/a0, b1/a0, b2/a0, a1/a0, a2/a0)。
     */
    void writeShelfCoeffsInPlace (SampleType* dst, SampleType gainDb) noexcept
    {
        const double A = std::pow (10.0, (double) gainDb / 40.0);
        const double val = (A + 1.0 / A) * oneOverSMinus1 + 2.0;
        const double alpha = sinW * 0.5 * std::sqrt (std::max (0.0, val));
        const double sqA = std::sqrt (A);

        double b0, b1, b2, a0, a1, a2;
        if (kind == Kind::HighShelf)
        {
            b0 =  A * ((A + 1) + (A - 1) * cosW + 2 * sqA * alpha);
            b1 = -2 * A * ((A - 1) + (A + 1) * cosW);
            b2 =  A * ((A + 1) + (A - 1) * cosW - 2 * sqA * alpha);
            a0 =       (A + 1) - (A - 1) * cosW + 2 * sqA * alpha;
            a1 =  2 *  ((A - 1) - (A + 1) * cosW);
            a2 =       (A + 1) - (A - 1) * cosW - 2 * sqA * alpha;
        }
        else
        {
            b0 =  A * ((A + 1) - (A - 1) * cosW + 2 * sqA * alpha);
            b1 =  2 * A * ((A - 1) - (A + 1) * cosW);
            b2 =  A * ((A + 1) - (A - 1) * cosW - 2 * sqA * alpha);
            a0 =       (A + 1) + (A - 1) * cosW + 2 * sqA * alpha;
            a1 = -2 *  ((A - 1) + (A + 1) * cosW);
            a2 =       (A + 1) + (A - 1) * cosW - 2 * sqA * alpha;
        }

        const double inv = 1.0 / a0;
        dst[0] = (SampleType) (b0 * inv);
        dst[1] = (SampleType) (b1 * inv);
        dst[2] = (SampleType) (b2 * inv);
        dst[3] = (SampleType) (a1 * inv);
        dst[4] = (SampleType) (a2 * inv);
    }

    Kind kind = Kind::LowShelf;
    int order = 1;
    double sampleRate = 48000.0;
    // RT-safe な in-place 更新用にキャッシュした geometry
    double cosW = 0.0, sinW = 0.0, oneOverSMinus1 = 0.0;
    SampleType currentGainDb = 0, currentCornerHz = 1000, currentSlope = 1;
    std::vector<juce::dsp::IIR::Filter<SampleType>> pre, post;
};
} // namespace kyohei::dsp
