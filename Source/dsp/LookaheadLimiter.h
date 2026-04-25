// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <vector>
#include <algorithm>
#include <cstdint>

namespace kyohei::dsp
{
/**
 * サンプル単位の look-ahead True-Peak リミッター。
 *
 * 設計:
 *   1. 入力 |x| に対し L サンプル幅の sliding-max（単調減少 deque, O(1) amortized）
 *      → 次 L サンプル以内の最大値 peak を常に得る
 *   2. required gain = min(1, threshold / peak)
 *   3. envelope: 下降は即座（sliding-max が look-ahead 時間を既に確保済みのため）、
 *      上昇は release 時定数で 1 次平滑
 *   4. さらに stage-2 の 1 次 LP を掛けて gain 曲線の角を丸める（C1/C2 化）
 *   5. audio は L サンプル遅延 → 出力 = delay[oldest] * smoothedGain
 *
 * オーバーサンプル段内で呼ぶこと（True-Peak ≒ ISP を捕捉するため）。
 * レイテンシ: L サンプル（OS レート基準）
 */
template <typename SampleType>
class LookaheadLimiter
{
public:
    /**
     * @param sampleRate        OS レートのサンプルレート (例: 48000 * 16 = 768000)
     * @param lookaheadSamples  OS レートでの look-ahead 長。0.5ms 相当を推奨
     * @param releaseMs         gain 復帰時定数 (base-rate 時間で指定)
     * @param smoothMs          stage-2 LP の時定数 (短め, 0.03〜0.1 ms 程度)
     */
    void prepare (double sampleRate, int lookaheadSamples,
                  double releaseMs = 50.0, double smoothMs = 0.05)
    {
        L = std::max (1, lookaheadSamples);
        audioDelay.assign ((size_t) (L + 1), SampleType (0));
        dqBuf.assign ((size_t) (L + 1), DqEntry{});
        writePos = 0;
        dqHead = 0;
        dqTail = 0;
        dqSize = 0;
        sampleIndex = 0;

        // release: 1 次 RC
        releaseCoeff = (SampleType) std::exp (-1.0 / (releaseMs * 0.001 * sampleRate));

        // stage-2 smoothing: 1 次 LP（両方向）
        smoothCoeff  = (SampleType) std::exp (-1.0 / (smoothMs  * 0.001 * sampleRate));

        envGR    = SampleType (1);
        smoothGR = SampleType (1);
    }

    void reset()
    {
        std::fill (audioDelay.begin(), audioDelay.end(), SampleType (0));
        dqHead = 0;
        dqTail = 0;
        dqSize = 0;
        sampleIndex = 0;
        writePos = 0;
        envGR    = SampleType (1);
        smoothGR = SampleType (1);
    }

    void setThreshold (SampleType t) noexcept { threshold = t; }

    int getLatencySamples() const noexcept { return L; }

    SampleType process (SampleType x) noexcept
    {
        const SampleType ax = std::abs (x);

        // --- sliding-max (monotone decreasing deque) ---
        while (dqSize > 0 && dqBack().val <= ax)
            dqPopBack();
        DqEntry e { ax, sampleIndex };
        dqPushBack (e);
        // 前方から古いエントリを除外（現在サンプルから L サンプル以上前のもの）
        while (dqSize > 0 && (sampleIndex - dqFront().idx) > L)
            dqPopFront();

        const SampleType peak = dqFront().val;

        // --- required gain reduction ---
        const SampleType reqGR = (peak > threshold) ? (threshold / peak) : SampleType (1);

        // --- stage-1: 下降は即時、上昇は release 時定数 ---
        if (reqGR < envGR)
            envGR = reqGR;
        else
            envGR = reqGR + releaseCoeff * (envGR - reqGR);

        // --- stage-2: 角丸め LP（双方向、対称） ---
        smoothGR = envGR + smoothCoeff * (smoothGR - envGR);

        // --- audio delay line ---
        audioDelay[(size_t) writePos] = x;
        const int readPos = (writePos + 1) % (L + 1);
        const SampleType outAudio = audioDelay[(size_t) readPos];
        writePos = readPos;
        ++sampleIndex;

        return outAudio * smoothGR;
    }

private:
    struct DqEntry { SampleType val = 0; std::int64_t idx = 0; };

    // 固定長リングバッファによる deque（RT セーフ）
    std::vector<DqEntry> dqBuf;
    int dqHead = 0, dqTail = 0, dqSize = 0;

    void dqPushBack (const DqEntry& e) noexcept
    {
        dqBuf[(size_t) dqTail] = e;
        dqTail = (dqTail + 1) % (int) dqBuf.size();
        ++dqSize;
    }
    void dqPopBack() noexcept
    {
        dqTail = (dqTail - 1 + (int) dqBuf.size()) % (int) dqBuf.size();
        --dqSize;
    }
    void dqPopFront() noexcept
    {
        dqHead = (dqHead + 1) % (int) dqBuf.size();
        --dqSize;
    }
    DqEntry& dqFront() noexcept { return dqBuf[(size_t) dqHead]; }
    DqEntry& dqBack()  noexcept
    {
        const int back = (dqTail - 1 + (int) dqBuf.size()) % (int) dqBuf.size();
        return dqBuf[(size_t) back];
    }

    std::vector<SampleType> audioDelay;
    int L = 0;
    int writePos = 0;
    std::int64_t sampleIndex = 0;

    SampleType threshold = SampleType (1);
    SampleType envGR = SampleType (1);
    SampleType smoothGR = SampleType (1);
    SampleType releaseCoeff = SampleType (0);
    SampleType smoothCoeff = SampleType (0);
};
} // namespace kyohei::dsp
