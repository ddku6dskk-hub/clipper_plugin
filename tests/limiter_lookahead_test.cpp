// 4.8 検証(本番ヘッダ直結): 修正後の LookaheadLimiter.h を実際に #include し、
// 病理ケース(厳密単調減少)で出力が threshold を超えない = peak 過小評価が起きないことを確認。
// バグ版(cap=L+1)ならGR不足で出力が threshold を明確に超過する。
// c++ -std=c++17 -O2 limiter_realheader_test.cpp -o /tmp/limreal && /tmp/limreal
#include "../Source/dsp/LookaheadLimiter.h"
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>

using kyohei::dsp::LookaheadLimiter;

static int run(const char* name, const std::vector<float>& in, int L, float thr) {
    LookaheadLimiter<float> lim;
    // release/smooth をほぼ即時にして「peak に対する理想GR」を観測しやすくする
    lim.prepare(768000.0, L, 0.0005, 0.0005);
    lim.setThreshold(thr);
    int N = (int) in.size();
    std::vector<float> out(N);
    for (int i = 0; i < N; ++i) out[i] = lim.process(in[i]);

    // 遅延 L サンプル後の領域で出力ピークを評価
    float maxOut = 0.0f;
    for (int i = L + 4; i < N; ++i) maxOut = std::max(maxOut, std::fabs(out[i]));
    bool ok = maxOut <= thr * 1.05f; // 5%マージン(release/smoothの過渡)
    printf("[%s] L=%d thr=%.3f maxOut=%.4f over=%.1f%% -> %s\n",
           name, L, thr, maxOut, (maxOut/thr - 1.0f) * 100.0f, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int main() {
    const int L = 32;
    const int N = L * 6;
    // 病理: 1.0から厳密単調減少 (threshold 0.2 を大きく超える領域から始まる)
    std::vector<float> dec(N);
    for (int i = 0; i < N; ++i) dec[i] = 1.0f - 0.002f * i;
    // 現実: 振動 + 緩やかな減衰
    std::vector<float> osc(N);
    for (int i = 0; i < N; ++i) osc[i] = std::sin(0.25f * i) * (0.9f - 0.001f * i);

    printf("=== 修正後ヘッダ(LookaheadLimiter.h)を直結検証 ===\n");
    int fail = 0;
    fail += run("decreasing(病理)", dec, L, 0.2f);
    fail += run("oscillating(現実)", osc, L, 0.2f);
    printf("\n%s\n", fail ? "FAIL: 出力がthresholdを超過(修正不十分)" : "PASS: 全ケースで出力がthreshold以下(修正有効)");
    return fail;
}
