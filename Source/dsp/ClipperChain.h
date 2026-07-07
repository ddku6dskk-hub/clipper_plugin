// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <juce_dsp/juce_dsp.h>
#include "Shaper.h"
#include "LinkedShelf.h"
#include "EnvelopeFollower.h"

namespace kyohei::dsp
{
/**
 * 完全な clipper チェーン: モード分岐 + linked shelf + shaper + OS。
 * 測定から導いたパラメータをそのまま反映する。
 *
 * サンプル単位で process() を呼ぶだけでよい。
 *
 * ① Open モード:
 *    pre_shelf(+5.96 dB HF @ 5979 Hz, slope 0.72) → shaper → post_shelf(鏡像)
 *
 * ② LF モード:
 *    env = peak_follower()
 *    od_db = max(0, 20·log10(env/T))
 *    g = clamp(0.66 - 0.657·od_db, -12, 0)
 *    pre_shelf(g LF @ 2604 Hz, slope 0.37, order 2) → shaper → post_shelf(鏡像)
 *
 * ③ B.Wall モード: shaper のみ
 */
template <typename SampleType>
class ClipperChain
{
public:
    enum class Mode { BrickWall, Open, LFClipper };

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        openShelf.prepare (spec, 1);
        lfShelf.prepare (spec, 2);
        envFollower.prepare (spec.sampleRate);

        openShelf.setShelf (LinkedShelf<SampleType>::Kind::HighShelf,
                            (SampleType) 5.96, (SampleType) 5979, (SampleType) 0.72);
        lfShelf.setShelf (LinkedShelf<SampleType>::Kind::LowShelf,
                          (SampleType) 0, (SampleType) 2604, (SampleType) 0.37);

        // threshold/knee 平滑化: 5ms 線形ランプ (input/output gain の applyGainRamp と
        // 同じ思想のジッパー対策。従来はブロック単位ステップで shaper カーブが段差変化した)。
        // reset 後の初回セットは即時反映なので、静的パラメータでは出力は従来と bit 一致。
        threshSmooth.reset (spec.sampleRate, 0.005);
        kneeSmooth.reset (spec.sampleRate, 0.005);
    }

    void reset()
    {
        openShelf.reset();
        lfShelf.reset();
        envFollower.reset();
        lastLfGainDb = (SampleType) 1000; // 番兵値に戻す
        lastShaperGain = (SampleType) 1;  // GR メーター用ゲインも初期化
        threshInit = false;               // 次の setThresholdDb/setKneeDb は即時反映
        kneeInit = false;
    }

    void setMode (Mode m)
    {
        if (m != mode)
        {
            // 旧モードで溜まった shelf/env の残留状態を破棄し、復帰時の挙動を決定論化する。
            // (波形整形カーブ自体が瞬時に変わるため切替の不連続は本質的に残る — 大手も同じ。
            //  これは「残留状態由来の余計な過渡」だけを排除する措置)
            // 係数次数は不変なので IIR::reset() は alloc なし = RT-safe。
            openShelf.reset();
            lfShelf.reset();
            envFollower.reset();
            lastLfGainDb = (SampleType) 1000; // LF 復帰時に係数を必ず再計算させる
        }
        mode = m;
    }
    /** threshold 設定。reset 後の初回は即時反映、以降は 5ms 線形ランプ (ジッパー対策)。 */
    void setThresholdDb (SampleType dB)
    {
        thresholdDb = dB;
        const SampleType tLin = std::pow ((SampleType) 10, dB / (SampleType) 20);
        if (threshInit)
        {
            threshSmooth.setTargetValue (tLin);
        }
        else
        {
            threshSmooth.setCurrentAndTargetValue (tLin);
            threshInit = true;
        }
        updateKneeTarget (false);
    }
    /** knee 設定。threshold と同様に平滑化される。 */
    void setKneeDb (SampleType dB)
    {
        kneeDb = dB;
        // knee 幅の線形換算: K_lin = T_lin·(1 − 10^(−kneeDb/20)) を threshold の上下に
        // 半分ずつ展開する。dB 軸で厳密に ±kneeDb/2 ではなく「threshold 比の線形幅」と
        // して定義した独自マッピング (実測フィット時からこの定義で一貫)。
        kneeFactor = (SampleType) 1 - std::pow ((SampleType) 10, -dB / (SampleType) 20);
        updateKneeTarget (true);
    }

    SampleType process (SampleType x) noexcept
    {
        // 平滑化パラメータをサンプル単位で前進 (非平滑時は目標値をそのまま返すだけで安価)
        const SampleType T = threshSmooth.getNextValue();
        const SampleType K = kneeSmooth.getNextValue();

        switch (mode)
        {
            case Mode::BrickWall:
            {
                const SampleType y = symmetricSoftClip (x, T, K);
                measureShaper (x, y);
                return y;
            }

            case Mode::Open:
            {
                const auto pre = openShelf.processPre (x);
                const auto shaped = symmetricSoftClip (pre, T, K);
                measureShaper (pre, shaped);
                return openShelf.processPost (shaped);
            }

            case Mode::LFClipper:
            {
                const SampleType env = envFollower.process (x);
                const SampleType od = env > T
                    ? (SampleType) 20 * std::log10 (env / T)
                    : (SampleType) 0;

                // 測定値: g(od) = max(-12, 0.66 - 0.657·od)
                SampleType g = (SampleType) 0.66 - (SampleType) 0.657 * od;
                g = juce::jlimit ((SampleType) -12, (SampleType) 0, g);

                // 0.05dB 以上変化した時のみ係数更新（RT-safe な in-place 書き込み）
                if (std::abs (g - lastLfGainDb) > (SampleType) 0.05)
                {
                    lfShelf.setGainDbFast (g);
                    lastLfGainDb = g;
                }

                const auto pre = lfShelf.processPre (x);
                const auto shaped = symmetricSoftClip (pre, T, K);
                measureShaper (pre, shaped);
                return lfShelf.processPost (shaped);
            }
        }
        return x;
    }

    // 直近 process() で shaper が実際に適用したゲイン係数 (≤1)。GR メーター用。
    // shaper はメモリレスなので入出力比がそのまま「潰した量」になる (時間ズレなし)。
    // shelf や OS の FIR/位相は GR に含めない (それらはゲインリダクションではない)。
    SampleType getLastShaperGain() const noexcept { return lastShaperGain; }

private:
    void measureShaper (SampleType in, SampleType out) noexcept
    {
        const SampleType ain = std::abs (in);
        if (ain > (SampleType) 1e-9)
        {
            const SampleType g = std::abs (out) / ain;
            lastShaperGain = g < (SampleType) 1 ? g : (SampleType) 1;
        }
        else
        {
            lastShaperGain = (SampleType) 1;
        }
    }

    /**
     * kneeLin 目標値の再計算 (kneeLin = 目標 T_lin × kneeFactor)。
     * kneeInit は setKneeDb 側でのみ立てる: reset 直後のブロックは
     * setThresholdDb → setKneeDb の順で呼ばれるため、setThresholdDb 経由の
     * スナップ (古い kneeFactor) を直後の setKneeDb が正しい値で上書きスナップし、
     * 初回ブロックの先頭サンプルから正確な knee で動く。
     */
    void updateKneeTarget (bool fromKneeSetter) noexcept
    {
        const SampleType kLin = threshSmooth.getTargetValue() * kneeFactor;
        if (kneeInit)
        {
            kneeSmooth.setTargetValue (kLin);
        }
        else
        {
            kneeSmooth.setCurrentAndTargetValue (kLin);
            if (fromKneeSetter)
                kneeInit = true;
        }
    }

    double sampleRate = 48000.0;
    Mode mode = Mode::BrickWall;
    SampleType thresholdDb = (SampleType) -6;
    SampleType kneeDb = (SampleType) 0;
    SampleType kneeFactor = (SampleType) 0;      // 1 − 10^(−kneeDb/20)
    SampleType lastLfGainDb = (SampleType) 1000; // 初回は必ず更新させるための番兵
    SampleType lastShaperGain = (SampleType) 1;  // 直近 shaper 適用ゲイン (GR メーター用)

    // threshold/knee の線形値スムーザー (5ms ランプ、prepare で reset)
    juce::SmoothedValue<SampleType, juce::ValueSmoothingTypes::Linear> threshSmooth, kneeSmooth;
    bool threshInit = false;  // reset 後の初回セットを即時反映にするフラグ
    bool kneeInit = false;

    LinkedShelf<SampleType> openShelf;
    LinkedShelf<SampleType> lfShelf;
    PeakFollower<SampleType> envFollower;
};
} // namespace kyohei::dsp
