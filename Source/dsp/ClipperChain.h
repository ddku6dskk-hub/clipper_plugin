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
    }

    void reset()
    {
        openShelf.reset();
        lfShelf.reset();
        envFollower.reset();
        lastLfGainDb = (SampleType) 1000; // 番兵値に戻す
    }

    void setMode (Mode m) { mode = m; }
    void setThresholdDb (SampleType dB)
    {
        thresholdDb = dB;
        thresholdLin = std::pow ((SampleType) 10, dB / (SampleType) 20);
        updateKneeLin();
    }
    void setKneeDb (SampleType dB)
    {
        kneeDb = dB;
        updateKneeLin();
    }

    SampleType process (SampleType x) noexcept
    {
        switch (mode)
        {
            case Mode::BrickWall:
                return symmetricSoftClip (x, thresholdLin, kneeLin);

            case Mode::Open:
            {
                auto y = openShelf.processPre (x);
                y = symmetricSoftClip (y, thresholdLin, kneeLin);
                y = openShelf.processPost (y);
                return y;
            }

            case Mode::LFClipper:
            {
                const SampleType env = envFollower.process (x);
                const SampleType od = env > thresholdLin
                    ? (SampleType) 20 * std::log10 (env / thresholdLin)
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

                auto y = lfShelf.processPre (x);
                y = symmetricSoftClip (y, thresholdLin, kneeLin);
                y = lfShelf.processPost (y);
                return y;
            }
        }
        return x;
    }

private:
    void updateKneeLin() noexcept
    {
        // 正しい dB→linear 変換: knee を threshold 上下に ±kneeDb/2 dB 分の幅で展開
        // K_lin = T_lin * (1 - 10^(-kneeDb/20))
        kneeLin = thresholdLin * ((SampleType) 1 - std::pow ((SampleType) 10, -kneeDb / (SampleType) 20));
    }

    double sampleRate = 48000.0;
    Mode mode = Mode::BrickWall;
    SampleType thresholdDb = (SampleType) -6;
    SampleType thresholdLin = (SampleType) 0.5;
    SampleType kneeDb = (SampleType) 0;
    SampleType kneeLin = (SampleType) 0;
    SampleType lastLfGainDb = (SampleType) 1000; // 初回は必ず更新させるための番兵

    LinkedShelf<SampleType> openShelf;
    LinkedShelf<SampleType> lfShelf;
    PeakFollower<SampleType> envFollower;
};
} // namespace kyohei::dsp
