// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <algorithm>

namespace kyohei::dsp
{
/**
 * 単純な peak envelope follower (sample accurate)。
 * attack/release は1次指数。
 * LF Clipper の動的 shelf 制御用途のため、~1ms 時定数で運用。
 */
template <typename SampleType>
class PeakFollower
{
public:
    void prepare (double sampleRate_)
    {
        sampleRate = sampleRate_;
        setAttackMs (attackMs);
        setReleaseMs (releaseMs);
        reset();
    }

    void reset() { env = SampleType (0); }

    void setAttackMs (double ms)
    {
        attackMs = ms;
        attackCoeff = (SampleType) std::exp (-1.0 / (ms * 0.001 * sampleRate));
    }

    void setReleaseMs (double ms)
    {
        releaseMs = ms;
        releaseCoeff = (SampleType) std::exp (-1.0 / (ms * 0.001 * sampleRate));
    }

    SampleType process (SampleType x) noexcept
    {
        const SampleType ax = std::abs (x);
        const SampleType coeff = ax > env ? attackCoeff : releaseCoeff;
        env = ax + coeff * (env - ax);
        return env;
    }

private:
    double sampleRate = 48000.0;
    double attackMs = 0.9;
    double releaseMs = 0.5;
    SampleType attackCoeff = 0, releaseCoeff = 0;
    SampleType env = 0;
};
} // namespace kyohei::dsp
