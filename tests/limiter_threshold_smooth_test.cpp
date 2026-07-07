// v1.0.8 threshold スムージング検証 (本番ヘッダ直結):
//   (1) prepare 後の初回 setThreshold は即時反映される (再生開始時に不要ランプが走らない)
//   (2) 2回目以降の setThreshold は 5ms 線形ランプで遷移し、ランプ終端で目標値に厳密到達する
//   (3) 遷移中の出力に段差 (ランプ勾配を超えるサンプル間ジャンプ = ジッパー) がない
// c++ -std=c++17 -O2 limiter_threshold_smooth_test.cpp -o /tmp/limsmooth && /tmp/limsmooth
#include "../Source/dsp/LookaheadLimiter.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

using kyohei::dsp::LookaheadLimiter;

int main()
{
    const double fs = 96000.0;
    const int L = 8;
    const int rampLen = (int) std::llround (0.005 * fs); // 480 samples
    int fail = 0;

    LookaheadLimiter<float> lim;
    // release/smooth をほぼ即時にして「threshold に対する理想出力」を観測しやすくする
    lim.prepare (fs, L, 0.0005, 0.0005);

    // --- (1) 初回 setThreshold は即時反映 ---
    lim.setThreshold (0.5f);
    float out = 0.0f;
    for (int i = 0; i < L + 32; ++i)
        out = lim.process (1.0f); // DC 1.0 入力
    {
        const bool ok = std::fabs (out - 0.5f) < 1e-3f;
        printf ("[initial-snap] out=%.6f (期待 0.5) -> %s\n", out, ok ? "PASS" : "FAIL");
        fail += ok ? 0 : 1;
    }

    // --- (2)(3) 2回目はランプ遷移: 単調 + 段差なし + 厳密到達 ---
    lim.setThreshold (0.25f);
    const int N = rampLen + 64;
    std::vector<float> ys ((size_t) N);
    for (int i = 0; i < N; ++i)
        ys[(size_t) i] = lim.process (1.0f);

    // 段差チェック: DC 入力なので出力 ≈ threshold(t)。1 サンプルあたりの変化は
    // ランプ勾配 (0.25/480 ≈ 5.2e-4) 近傍に収まるはず。旧実装 (ブロックステップ) なら
    // 1 サンプルで 0.25 落ちる箇所が出る。余裕を見て勾配の 4 倍を上限とする。
    const float slopeLimit = 4.0f * (0.5f - 0.25f) / (float) rampLen;
    float maxJump = 0.0f;
    bool monotone = true;
    for (int i = 1; i < N; ++i)
    {
        const float jump = std::fabs (ys[(size_t) i] - ys[(size_t) i - 1]);
        maxJump = std::max (maxJump, jump);
        if (ys[(size_t) i] > ys[(size_t) i - 1] + 1e-6f)
            monotone = false;
    }
    {
        const bool ok = maxJump <= slopeLimit && monotone;
        printf ("[ramp-smooth]  maxJump=%.2e (上限 %.2e) monotone=%d -> %s\n",
                maxJump, slopeLimit, (int) monotone, ok ? "PASS" : "FAIL");
        fail += ok ? 0 : 1;
    }

    // 厳密到達チェック: ランプ終端で targetThreshold へスナップするので誤差ゼロ近傍
    {
        const float settled = ys[(size_t) (N - 1)];
        const bool ok = std::fabs (settled - 0.25f) < 1e-6f;
        printf ("[exact-settle] settled=%.8f (期待 0.25) -> %s\n", settled, ok ? "PASS" : "FAIL");
        fail += ok ? 0 : 1;
    }

    printf ("\n%s\n", fail ? "FAIL" : "PASS: threshold スムージング 3 項目すべて OK");
    return fail;
}
