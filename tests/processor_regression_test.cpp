// SPDX-License-Identifier: GPL-3.0-or-later
// KyoheiClipperProcessor 統合テスト (JUCE リンク必須。既存 3 テストと違い本番 Processor を通す)。
//
// 【背景】既存の 3 テストは DSP ヘッダ直結なので、Processor / Editor 層のバグ
// (状態保存・メーター/表示・host bypass) を構造的に検出できない。
// 2026-09-05 の Codex 検品で見つかった 3 件は全てこの層にあった:
//
//   F1  パラメータ変更直後の保存が古い値になる (apvts.state 直書き)
//       実測: Threshold -12dB に変更した直後の保存で Clipper -6 / Slammer -3 が入った
//   F2  完全 bypass 中もビジュアライザに「削られた分 (琥珀)」が出続ける
//       (GR メーターは bypass 補正済みなので、同じ画面で表示同士が食い違っていた)
//   B1  processBlockBypassed から復帰すると bypass 前の音が出る
//       実測: DC 0.1 処理 → 無音を 20 ブロック hard bypass → 復帰 1 ブロック目で
//             無音入力なのに出力ピーク 0.0999 (Clipper) / 0.1135 (Slammer)
//
// 2026-09-11 の Codex 修正レビューで、hard bypass の「停止＋復帰時に作り直し」に
// 残っていた穴を T5/T7/T8/T9 で、テスト自体の検出漏れを T3/T4 で塞いだ:
//
//   R1  復帰直後のチャンクだけ、bypass 中に変えた threshold/knee が効かない      → T5
//   R2  dry 保持がブロック単位に切り上がり、復帰時間がブロック長で伸びる        → T7
//   R3  prepareToPlay が復帰フラグを消さず、前回の bypass 状態が漏れる           → T8
//   R4  メーターとビジュアライザで GR を集計する時間区間が違う                   → T4
//   R5  書きかけのビジュアライザフレームが hard bypass をまたいで古い GR を出す  → T9
//   R6  T3 は全 dry 区間しか見ておらず wet の残留を検出できなかった              → T3
//
// 2026-09-24 の検品で、T1-T9 は出音を「新規インスタンスとの一致」でしか見ておらず、実装と独立な
// 物差しを持っていないと分かった (シェイパー/リミッターを音から外す・Output ゲインを消す、の
// 3 変異が全部素通り)。その物差しを T10 に置き、ホスト由来の呼び出し 2 種の穴を T11/T12 で塞いだ:
//
//   F2  Slammer の OS 群遅延 59.5 を 60 と報告し、出音が 0.5 サンプル早い         → T10 (I1)
//   F3  長さ 0 の processBlock が Input/Output のゲインランプを使い切る            → T12
//   F4  reset() 未実装で、ホストのリセット後も前の音とリミッター減衰が残る        → T11
//
// なお T3/T5-T9 が見ている processBlockBypassed は、JUCE の AAX/AU/VST3 ラッパーでは
// getBypassParameter() を返している限り呼ばれない (ホストの bypass は bypass パラメータ =
// soft bypass になる。AAX.cpp の process 参照)。ラッパーを通らずに呼ばれたときの保険の検証。
//
// ビルド/実行は tests/run_tests.sh を使う (SharedCode 静的ライブラリが必要なので、
// 先に一度 CMake で Release ビルドしておくこと)。
#include "PluginProcessor.h"
#include <cstdio>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#if KYOHEI_SLAMMER
 static const char* kProductName = "K Slammer";
#else
 static const char* kProductName = "K Clipper";
#endif

static int failures = 0;
static void check (bool ok, const char* what)
{
    printf ("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++failures;
}

static void setParam (KyoheiClipperProcessor& p, const char* id, float v)
{
    auto* a = p.apvts.getParameter (id);
    a->setValueNotifyingHost (a->convertTo0to1 (v));
}

// 復帰クロスフェードの長さ。bypassMix.reset (sampleRate, 0.015) の段数と同じ。
static int fadeSamples (double fs) { return (int) std::floor (0.015 * fs); }

// ---------------- 連続信号を任意のブロック長で流す道具 ----------------
// 呼び出しをまたいで位相が飛ばないよう、サンプル位置を持ち越す。
struct Signal
{
    double fs = 48000.0;
    long long n = 0;
    float amp = 0.5f;
    double amHz = 0.0;   // 0 以外なら振幅を揺らして GR を時間変動させる

    float next()
    {
        const double t = (double) n++ / fs;
        double a = amp;
        if (amHz > 0.0)
            a *= 0.6 + 0.4 * std::sin (juce::MathConstants<double>::twoPi * amHz * t);
        return (float) (a * (0.7 * std::sin (juce::MathConstants<double>::twoPi * 220.0 * t)
                           + 0.3 * std::sin (juce::MathConstants<double>::twoPi * 1370.0 * t)));
    }
};

// count サンプルを lens の長さ列 (循環) で流す。L/R は同じ信号。
// in / out を渡すと ch0 の入力・出力を追記する。
static void runSegment (KyoheiClipperProcessor& p, Signal& sig, int count,
                        const std::vector<int>& lens, bool hardBypass,
                        std::vector<float>* in, std::vector<float>* out)
{
    juce::MidiBuffer midi;
    size_t li = 0;
    int done = 0;
    while (done < count)
    {
        const int len = juce::jmin (lens[li++ % lens.size()], count - done);
        juce::AudioBuffer<float> b (2, len);
        for (int i = 0; i < len; ++i)
        {
            const float v = sig.next();
            b.setSample (0, i, v);
            b.setSample (1, i, v);
            if (in != nullptr)
                in->push_back (v);
        }
        if (hardBypass)
            p.processBlockBypassed (b, midi);
        else
            p.processBlock (b, midi);
        if (out != nullptr)
            for (int i = 0; i < len; ++i)
                out->push_back (b.getSample (0, i));
        done += len;
    }
}

// ---------------- F1: 変更直後の保存 ----------------
// 「タイマー同期を待たずに保存」を必ず含めること。数十ms 待つテストでは取りこぼしが隠れる。
static void testStateRoundTrip()
{
    printf ("T1: state save/restore immediately after a parameter change\n");
    KyoheiClipperProcessor ref;
    bool ok = true;
    int checked = 0;

    for (auto* param : ref.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param);
        if (ranged == nullptr)
            continue;
        const auto id = ranged->paramID;
        const auto range = ref.apvts.getParameterRange (id);
        const float cur = ref.apvts.getRawParameterValue (id)->load();

        // 既定値とは違う「動かした値」を選ぶ (同値だと保存漏れを検出できない)
        float want = range.snapToLegalValue (range.start + (range.end - range.start) * 0.25f);
        if (std::abs (want - cur) < 1e-3f)
            want = range.snapToLegalValue (range.start + (range.end - range.start) * 0.75f);

        KyoheiClipperProcessor p;
        p.apvts.getParameter (id)->setValueNotifyingHost (range.convertTo0to1 (want));

        juce::MemoryBlock block;
        p.getStateInformation (block);                     // ← タイマーを待たずに保存
        KyoheiClipperProcessor restored;
        restored.setStateInformation (block.getData(), (int) block.getSize());

        const float got = restored.apvts.getRawParameterValue (id)->load();
        ++checked;
        if (std::abs (got - want) > 1e-3f)
        {
            printf ("     %s: want %g, restored %g\n", id.toRawUTF8(), want, got);
            ok = false;
        }
    }
    printf ("     (%d parameters checked)\n", checked);
    check (checked > 0 && ok, "every parameter survives an immediate save/restore");
}

// ---------------- F2: bypass 中のビジュアライザ ----------------
static float lastVisGr (const KyoheiClipperProcessor& p)
{
    const int idx = (p.visWritePos.load (std::memory_order_acquire)
                     + KyoheiClipperProcessor::kVisFrames - 1) % KyoheiClipperProcessor::kVisFrames;
    return p.visGrDb[0][(size_t) idx].load (std::memory_order_relaxed);
}

static void feedTone (KyoheiClipperProcessor& p, int blocks, int blockLen, double fs, float amp)
{
    juce::AudioBuffer<float> b (2, blockLen);
    juce::MidiBuffer m;
    for (int k = 0; k < blocks; ++k)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < blockLen; ++i)
                b.setSample (c, i, amp * (float) std::sin (juce::MathConstants<double>::twoPi
                                                           * 1000.0 * (k * blockLen + i) / fs));
        p.processBlock (b, m);
    }
}

static void testVisualiserFollowsBypass()
{
    printf ("T2: analyser gain-reduction lane follows the Bypass crossfade\n");

    KyoheiClipperProcessor bypassed;
    setParam (bypassed, "threshold", -12.0f);
    setParam (bypassed, "knee", 0.0f);
    setParam (bypassed, "inputGain", 12.0f);
    setParam (bypassed, "bypass", 1.0f);
    bypassed.prepareToPlay (48000.0, 480);
    feedTone (bypassed, 120, 480, 48000.0, 0.1f);
    printf ("     bypassed: vis GR %.3f dB / meter GR %.3f dB\n",
            lastVisGr (bypassed), bypassed.grPeakDb.load());
    check (lastVisGr (bypassed) < 0.05f, "no gain reduction drawn while fully bypassed");

    KyoheiClipperProcessor active;
    setParam (active, "threshold", -12.0f);
    setParam (active, "knee", 0.0f);
    setParam (active, "inputGain", 12.0f);
    active.prepareToPlay (48000.0, 480);
    feedTone (active, 120, 480, 48000.0, 0.1f);
    printf ("     active:   vis GR %.3f dB / meter GR %.3f dB\n",
            lastVisGr (active), active.grPeakDb.load());
    check (lastVisGr (active) > 0.05f, "gain reduction is still drawn while active");
}

// ---------------- B1: hard bypass からの復帰 ----------------
static void testHardBypassResume()
{
    printf ("T3: returning from processBlockBypassed plays no stale audio\n");

    // 復帰区間の出力を**サンプル単位で期待値と完全一致**させる。期待値は
    //   先頭 D サンプル : 遅延 dry
    //   その後 15ms     : 「復帰点から同じ入力を流した新規インスタンスの出力 (= 正しい wet)」と
    //                     dry を、本体と同じ SmoothedValue・同じ式で混ぜたもの
    //   フェード後      : 新規インスタンスの出力そのもの
    // 【なぜ無音で見ないか】以前は「無音で復帰して最初の 64 サンプルが無音」を見ていたが、
    // その区間は全 dry なので wet の残留が写らず (2026-09-11 R6)、フェード完了まで無音で見ても
    // 残ったリミッターの減衰や LF のエンベロープは無音に掛かるだけで出力に出ない
    // (実測: 復帰時に作り直さない変異を素通りした)。鳴らしたまま戻り、正しい wet と突き合わせる。
    const double fs = 48000.0;
    const int len = 64;
    float worstResume = 0.0f;
    for (float mode : { 0.0f, 1.0f, 2.0f })
    {
        const auto setup = [mode] (KyoheiClipperProcessor& proc)
        {
            setParam (proc, "mode", mode);
            setParam (proc, "threshold", -12.0f);
            setParam (proc, "knee", 0.0f);
            setParam (proc, "inputGain", 12.0f);
        };
        KyoheiClipperProcessor p;
        setup (p);
        p.prepareToPlay (fs, len);
        const int D = p.getLatencySamples();

        Signal sig;
        sig.amp = 0.8f;
        std::vector<float> in, out;
        runSegment (p, sig, 40 * len, { len }, false, &in, &out);
        runSegment (p, sig, 5 * len, { len }, true, &in, &out);   // 短い bypass: wet 側の状態がまだ濃い
        const int resumeAt = (int) in.size();
        const long long resumeN = sig.n;
        const int window = D + fadeSamples (fs) + 4096;
        runSegment (p, sig, window, { len }, false, &in, &out);

        KyoheiClipperProcessor q;
        setup (q);
        q.prepareToPlay (fs, len);
        Signal qs;
        qs.amp = 0.8f;
        qs.n = resumeN;
        std::vector<float> wet;
        runSegment (q, qs, window, { len }, false, nullptr, &wet);

        juce::SmoothedValue<float> mix;
        mix.reset (fs, 0.015);
        mix.setCurrentAndTargetValue (1.0f);
        for (int n = 0; n < window; ++n)
        {
            const float d = in[(size_t) (resumeAt + n - D)];
            float expect = d;
            if (n >= D)
            {
                if (n == D)
                    mix.setTargetValue (0.0f);
                const float mv = mix.getNextValue();
                expect = wet[(size_t) n] * (1.0f - mv) + d * mv;
            }
            worstResume = juce::jmax (worstResume, std::abs (out[(size_t) (resumeAt + n)] - expect));
        }
    }
    printf ("     worst |resume - (dry hold, crossfade to a fresh wet, fresh wet)| = %.3e\n", worstResume);
    check (worstResume == 0.0f, "no stale audio anywhere in the resume (hold, fade and after, all modes)");

    // 素通しそのものは壊していないこと (latency 整合の bit-exact コピー)
    KyoheiClipperProcessor q;
    q.prepareToPlay (48000.0, 64);
    const int D = q.getLatencySamples();
    juce::AudioBuffer<float> c2 (2, 64);
    juce::MidiBuffer m2;
    std::mt19937 rng (4);
    std::uniform_real_distribution<float> dist (-0.4f, 0.4f);
    std::vector<float> in, out;
    for (int k = 0; k < 40; ++k)
    {
        for (int i = 0; i < 64; ++i)
        {
            const float v = dist (rng);
            c2.setSample (0, i, v);
            c2.setSample (1, i, v);
            in.push_back (v);
        }
        q.processBlockBypassed (c2, m2);
        for (int i = 0; i < 64; ++i)
            out.push_back (c2.getSample (0, i));
    }
    double worst = 0.0;
    for (int n = D; n < (int) out.size(); ++n)
        worst = juce::jmax (worst, (double) std::abs (out[(size_t) n] - in[(size_t) (n - D)]));
    printf ("     hard bypass passthrough error = %.3e (latency %d)\n", worst, D);
    check (worst == 0.0, "hard bypass is still a bit-exact latency-aligned passthrough");

    // prepare より長いブロックと NaN/Inf (契約違反ホスト対策が効いていること)
    KyoheiClipperProcessor r;
    r.prepareToPlay (48000.0, 64);
    juce::AudioBuffer<float> c3 (2, 256);
    juce::MidiBuffer m3;
    c3.clear();
    c3.setSample (0, 0, std::numeric_limits<float>::quiet_NaN());
    c3.setSample (1, 1, std::numeric_limits<float>::infinity());
    r.processBlockBypassed (c3, m3);
    bool finite = true;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 256; ++i)
            if (! std::isfinite (c3.getSample (c, i)))
                finite = false;
    check (finite, "oversized block with NaN/Inf leaves no non-finite output");
}

// ---------------- メーターとビジュアライザ ----------------

// from 以降に公開されたフレームの GR 最大 (L/R の大きい方)。frames に公開数を返す。
static float visGrSince (const KyoheiClipperProcessor& p, int from, int& frames)
{
    const int to = p.visWritePos.load (std::memory_order_acquire);
    frames = (to - from + KyoheiClipperProcessor::kVisFrames) % KyoheiClipperProcessor::kVisFrames;
    float mx = 0.0f;
    for (int k = 0; k < frames; ++k)
    {
        const auto w = (size_t) ((from + k) % KyoheiClipperProcessor::kVisFrames);
        for (size_t c = 0; c < 2; ++c)
            mx = juce::jmax (mx, p.visGrDb[c][w].load (std::memory_order_relaxed));
    }
    return mx;
}

// フェード途中では、ビジュアライザとメーターの GR が食い違っていた。
//  - 2026-09-08: メーターが dB 値に wet 比率を掛けていた (g=0.1・m=0.5 で 5.19dB 対 10dB)
//  - 2026-09-11 R4: 式を揃えても、メーターは「ブロック最小ゲイン × ブロック末尾の mix」、
//    ビジュアライザは「サンプルごとの実効ゲインの最小」で、**集計する区間が違った**
//    (フェード開始フレームで 3.99dB 対 1.14dB)。
// 以前のこのテストは vis(k) と meter(k-1) を比べていてズレを隠していた。
// ここでは**同じサンプル区間**を比べる: 1 フレーム (480) を流す前にメーターを空にし、
// 流し終えたらその間に公開された 1 フレームとメーターを突き合わせる。
static void testMeterMatchesVisualiserDuringFade()
{
    printf ("T4: GR meter and analyser agree over the same span during the bypass crossfade\n");

    const double fs = 48000.0;
    const int frameLen = 480;                       // visFrameLen (10ms)
    float worst = 0.0f;
    int compared = 0;
    bool frameMisaligned = false;
    bool sawFadeOut = true, sawFadeIn = true;

    for (int blockLen : { 480, 160 })               // フレーム境界 = ブロック境界 / 1 フレーム = 3 ブロック
    {
        KyoheiClipperProcessor p;
        setParam (p, "threshold", -12.0f);
        setParam (p, "knee", 0.0f);
        setParam (p, "inputGain", 12.0f);
        p.prepareToPlay (fs, blockLen);
        Signal sig;
        sig.fs = fs;
        sig.amp = 0.4f;
        sig.amHz = 7.0;                              // GR を時間変動させる
        runSegment (p, sig, 40 * frameLen, { blockLen }, false, nullptr, nullptr);

        for (float target : { 1.0f, 0.0f })          // bypass へフェード → active へフェード
        {
            setParam (p, "bypass", target);
            std::vector<float> meters;
            for (int f = 0; f < 6; ++f)
            {
                const int from = p.visWritePos.load (std::memory_order_acquire);
                p.grPeakDb.store (-1000.0f);         // メーターは peak-hold なのでフレームごとに空にする
                runSegment (p, sig, frameLen, { blockLen }, false, nullptr, nullptr);
                int frames = 0;
                const float vis = visGrSince (p, from, frames);
                const float meter = p.grPeakDb.load();
                if (frames != 1 || meter < -900.0f)
                {
                    frameMisaligned = true;
                    continue;
                }
                ++compared;
                meters.push_back (meter);
                worst = juce::jmax (worst, std::abs (vis - meter));
            }
            // フェードが本当に走ったこと: bypass 方向は GR あり → 0、active 方向は 0 → GR あり
            if (meters.size() == 6)
            {
                if (target > 0.5f) sawFadeOut = sawFadeOut && meters[0] > 0.05f && meters[3] < 0.01f;
                else               sawFadeIn  = sawFadeIn  && meters[0] > 0.05f && meters[5] > 0.05f;
            }
            else
            {
                sawFadeOut = sawFadeIn = false;
            }
        }
    }
    printf ("     worst |vis - meter| over identical spans = %.4f dB (%d frames compared)\n",
            worst, compared);
    check (! frameMisaligned && compared == 24, "each compared span published exactly one analyser frame");
    check (sawFadeOut && sawFadeIn, "the crossfade ran in both directions");
    check (worst < 0.01f, "meter and analyser report the same GR for the same samples");
}

// ---------------- hard bypass 停止＋作り直し ----------------

// R1: hard bypass 中に設定を変えて戻したとき、**復帰の最初のサンプルから**新しい設定で
// 動いていること。旧実装は「設定→reset」の順で、ClipperChain::reset() が平滑化の
// 初回フラグだけ下ろして値を残すため、最初のチャンクが旧 threshold/knee からランプした。
// 参照は「最初から新設定で鳴らし、同じ入力で同じだけ bypass → 復帰」したインスタンス。
// dry 経路は設定に依存しないので、復帰区間 (dry 保持・フェード・その後) は丸ごと一致するはず。
static void testResumeUsesLatestSettings()
{
    printf ("T5: settings changed during hard bypass are in effect from the first resumed sample\n");

    struct Setting { float mode, thr, knee, inGain; };
    const Setting pairs[][2] = {
        { { 0.0f, -18.0f,  0.0f, 12.0f }, { 0.0f,   0.0f, 12.0f, 0.0f } },   // B.Wall: 深い→浅い
        { { 1.0f,   0.0f, 12.0f,  0.0f }, { 1.0f, -18.0f,  0.0f, 12.0f } },  // Open:   浅い→深い
        { { 2.0f, -18.0f,  0.0f, 12.0f }, { 2.0f,  -3.0f,  6.0f,  6.0f } },  // LF
        { { 1.0f,  -6.0f,  3.0f,  6.0f }, { 2.0f, -15.0f,  9.0f,  9.0f } },  // Open → LF
    };
    const auto apply = [] (KyoheiClipperProcessor& p, const Setting& s)
    {
        setParam (p, "mode", s.mode);
        setParam (p, "threshold", s.thr);
        setParam (p, "knee", s.knee);
        setParam (p, "inputGain", s.inGain);
    };

    const std::vector<int> lens { 128 };
    float worst = 0.0f;
    for (const auto& pr : pairs)
    {
        KyoheiClipperProcessor a, b;
        apply (a, pr[0]);
        apply (b, pr[1]);
        a.prepareToPlay (48000.0, 128);
        b.prepareToPlay (48000.0, 128);
        Signal sa, sb;

        runSegment (a, sa, 40 * 128, lens, false, nullptr, nullptr);
        runSegment (b, sb, 40 * 128, lens, false, nullptr, nullptr);

        runSegment (a, sa, 10 * 128, lens, true, nullptr, nullptr);
        apply (a, pr[1]);                           // bypass 中に設定を変える
        runSegment (a, sa, 10 * 128, lens, true, nullptr, nullptr);
        runSegment (b, sb, 20 * 128, lens, true, nullptr, nullptr);

        const int window = a.getLatencySamples() + fadeSamples (48000.0) + 2048;
        std::vector<float> oa, ob;
        runSegment (a, sa, window, lens, false, nullptr, &oa);
        runSegment (b, sb, window, lens, false, nullptr, &ob);
        for (size_t n = 0; n < oa.size(); ++n)
            worst = juce::jmax (worst, std::abs (oa[n] - ob[n]));
    }
    printf ("     worst |changed while bypassed - set from the start| over the whole resume = %.3e\n",
            worst);
    check (worst == 0.0f, "resume uses the latest settings from its first sample (all modes, both directions)");
}

// hard bypass を外した瞬間に、非ゼロの dry から空の wet へ直結するとクリックになる。
// 「レイテンシ分は dry を保持 → 15ms でクロスフェード」が効いているかを、
// 復帰境界の隣接サンプル差が定常時を超えないことで見る。
static void testResumeHasNoClick()
{
    printf ("T6: leaving hard bypass does not produce a step discontinuity\n");

    const int len = 128;
    const double fs = 48000.0;
    juce::MidiBuffer midi;
    KyoheiClipperProcessor p;
    setParam (p, "threshold", -6.0f);
    setParam (p, "inputGain", 6.0f);
    p.prepareToPlay (fs, len);

    long long n = 0;
    auto fill = [&] (juce::AudioBuffer<float>& b)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < len; ++i)
                b.setSample (c, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi
                                                             * 220.0 * (double) (n + i) / fs));
        n += len;
    };

    // 定常 active の隣接サンプル差 (基準)
    float steady = 0.0f;
    float last = 0.0f;
    for (int k = 0; k < 60; ++k)
    {
        juce::AudioBuffer<float> b (2, len);
        fill (b);
        p.processBlock (b, midi);
        if (k >= 20)
            for (int i = 0; i < len; ++i)
            {
                steady = juce::jmax (steady, std::abs (b.getSample (0, i) - last));
                last = b.getSample (0, i);
            }
        else
            last = b.getSample (0, len - 1);
    }

    for (int k = 0; k < 30; ++k)                    // 鳴らしたまま hard bypass
    {
        juce::AudioBuffer<float> b (2, len);
        fill (b);
        p.processBlockBypassed (b, midi);
        last = b.getSample (0, len - 1);
    }

    float boundary = 0.0f;                          // 復帰境界〜フェード完了まで
    for (int k = 0; k < 12; ++k)
    {
        juce::AudioBuffer<float> b (2, len);
        fill (b);
        p.processBlock (b, midi);
        for (int i = 0; i < len; ++i)
        {
            boundary = juce::jmax (boundary, std::abs (b.getSample (0, i) - last));
            last = b.getSample (0, i);
        }
    }
    printf ("     max |x[n]-x[n-1]|: steady %.4f / across resume %.4f\n", steady, boundary);
    check (boundary <= steady * 1.5f + 1.0e-4f,
           "no step at the bypass-to-active boundary (dry hold + crossfade)");
}

// R2: 復帰時の dry 保持は**ブロック長によらず**ちょうど D (= 報告レイテンシ) サンプル。
// 旧実装は保持残数をチャンク単位で減らしていたので、512 で 25.7ms、2048 で 57.7ms かかった。
// 期待: 先頭 D サンプルは遅延 dry と完全一致、D 番目からフェード開始、D + 15ms 以降は
// 「復帰点から同じ入力を流した新規インスタンス」と完全一致 (= wet へ戻り切っている)。
static void testResumeHoldIsSampleAccurate()
{
    printf ("T7: resume holds dry for exactly the latency, whatever the block size\n");

    const double fs = 48000.0;
    const int fade = fadeSamples (fs);
    struct Case { const char* name; int prepareLen; std::vector<int> lens; };
    const std::vector<Case> cases = {
        { "32",                  32,   { 32 } },
        { "64",                  64,   { 64 } },
        { "128",                 128,  { 128 } },
        { "512",                 512,  { 512 } },
        { "2048",                2048, { 2048 } },
        { "variable",            512,  { 37, 5, 300, 1, 512, 64 } },
        { "oversized (chunked)", 256,  { 1000 } },
    };

    bool allOk = true;
    for (const auto& cs : cases)
    {
        const auto setup = [] (KyoheiClipperProcessor& p)
        {
            setParam (p, "threshold", -12.0f);
            setParam (p, "knee", 0.0f);
            setParam (p, "inputGain", 12.0f);
        };
        KyoheiClipperProcessor p;
        setup (p);
        p.prepareToPlay (fs, cs.prepareLen);
        const int D = p.getLatencySamples();

        Signal sig;
        std::vector<float> in, out;
        runSegment (p, sig, 8192, cs.lens, false, &in, &out);
        runSegment (p, sig, 4096, cs.lens, true,  &in, &out);
        const int resumeAt = (int) in.size();
        const long long resumeN = sig.n;
        const int window = D + fade + 4096;
        runSegment (p, sig, window, cs.lens, false, &in, &out);

        KyoheiClipperProcessor q;                    // 復帰点から始めた新規インスタンス
        setup (q);
        q.prepareToPlay (fs, cs.prepareLen);
        Signal qs;
        qs.n = resumeN;
        std::vector<float> qout;
        runSegment (q, qs, window, cs.lens, false, nullptr, &qout);

        int firstNotDry = -1;
        for (int n = 0; n < window && firstNotDry < 0; ++n)
            if (out[(size_t) (resumeAt + n)] != in[(size_t) (resumeAt + n - D)])
                firstNotDry = n;

        int firstMismatchAfterFade = -1;
        for (int n = D + fade; n < window && firstMismatchAfterFade < 0; ++n)
            if (out[(size_t) (resumeAt + n)] != qout[(size_t) n])
                firstMismatchAfterFade = n;

        const bool ok = firstNotDry == D && firstMismatchAfterFade < 0;
        printf ("     block %-20s latency %3d: fade starts at %5d, %s\n", cs.name, D, firstNotDry,
                firstMismatchAfterFade < 0 ? "fully wet from latency+15ms"
                                           : "still differs from a fresh instance after latency+15ms");
        allOk = allOk && ok;
    }
    check (allOk, "dry is held for exactly the latency, then fades to wet within 15 ms");
}

// R3: prepareToPlay を挟んだら、それまでの hard bypass / 復帰の途中状態を持ち越さないこと。
// 期待: 同じ設定・同じ入力の新規インスタンスと完全一致 (サンプルレート変更も含む)。
static void testPrepareClearsResumeState()
{
    printf ("T8: prepareToPlay discards any pending hard-bypass resume\n");

    const auto setup = [] (KyoheiClipperProcessor& p)
    {
        setParam (p, "threshold", -12.0f);
        setParam (p, "inputGain", 12.0f);
    };
    const char* stateNames[] = { "in hard bypass", "holding dry", "crossfading" };

    float worst = 0.0f;
    for (int state = 0; state < 3; ++state)
    {
        for (double newFs : { 48000.0, 96000.0 })
        {
            KyoheiClipperProcessor p;
            setup (p);
            p.prepareToPlay (48000.0, 32);
            Signal sig;
            runSegment (p, sig, 32 * 20, { 32 }, false, nullptr, nullptr);
            runSegment (p, sig, 32 * 10, { 32 }, true,  nullptr, nullptr);
            if (state >= 1)
                runSegment (p, sig, 32, { 32 }, false, nullptr, nullptr);      // 32 < latency: 保持中
            if (state == 2)
                runSegment (p, sig, p.getLatencySamples() + 100, { 32 }, false, nullptr, nullptr);

            p.prepareToPlay (newFs, 128);
            KyoheiClipperProcessor q;
            setup (q);
            q.prepareToPlay (newFs, 128);

            Signal s1, s2;
            s1.fs = s2.fs = newFs;
            std::vector<float> op, oq;
            runSegment (p, s1, 128 * 40, { 128 }, false, nullptr, &op);
            runSegment (q, s2, 128 * 40, { 128 }, false, nullptr, &oq);
            float w = 0.0f;
            for (size_t n = 0; n < op.size(); ++n)
                w = juce::jmax (w, std::abs (op[n] - oq[n]));
            if (w != 0.0f)
                printf ("     re-prepare while %s -> %.0f Hz: max diff vs fresh instance %.3e\n",
                        stateNames[state], newFs, w);
            worst = juce::jmax (worst, w);
        }
    }
    printf ("     worst |re-prepared - fresh| = %.3e\n", worst);
    check (worst == 0.0f, "a re-prepared instance behaves exactly like a fresh one");
}

// R5: 書きかけのビジュアライザフレームが hard bypass をまたいで残り、無音で復帰したのに
// bypass 前の GR (実測 16dB) を載せたフレームが公開されていた。
static void testAnalyserFrameDoesNotSpanHardBypass()
{
    printf ("T9: a half-built analyser frame is not carried across hard bypass\n");

    KyoheiClipperProcessor p;
    setParam (p, "threshold", -12.0f);
    setParam (p, "knee", 0.0f);
    setParam (p, "inputGain", 12.0f);
    p.prepareToPlay (48000.0, 128);

    juce::AudioBuffer<float> b (2, 128);
    juce::MidiBuffer m;
    for (int k = 0; k < 40; ++k)                     // 40×128 = 10 フレーム + 320 サンプルの書きかけ
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 128; ++i)
                b.setSample (c, i, 0.5f);
        p.processBlock (b, m);
    }
    for (int k = 0; k < 20; ++k) { b.clear(); p.processBlockBypassed (b, m); }

    const int from = p.visWritePos.load (std::memory_order_acquire);
    for (int k = 0; k < 8; ++k) { b.clear(); p.processBlock (b, m); }   // 無音で復帰 (2 フレーム分)
    int frames = 0;
    const float gr = visGrSince (p, from, frames);
    float inPeak = -100.0f;
    for (int k = 0; k < frames; ++k)
        for (size_t c = 0; c < 2; ++c)
            inPeak = juce::jmax (inPeak, p.visInDb[c][(size_t) ((from + k) % KyoheiClipperProcessor::kVisFrames)]
                                             .load (std::memory_order_relaxed));
    printf ("     frames published after resume: %d, max GR %.3f dB, max input %.1f dBFS\n",
            frames, gr, inPeak);
    check (frames > 0 && gr < 0.05f && inPeak <= -99.0f,
           "frames after a silent resume carry no pre-bypass GR or level");
}

// ---------------- 2026-09-24 検品: active 経路の出音 ----------------
// 実装と独立な物差し (正弦の位相と振幅、閾値との大小) だけで判定する。新規インスタンスとの
// 比較と違い、両方に同じ欠陥が入っていても見逃さない。
// 【NaN】std::max / jmax は NaN を黙って捨てるので、非有限サンプルは別枠で数えて合否に入れる。

// 正弦 1 本 (48 kHz) を L/R に入れて N サンプル流し、ch0 の出力を返す。最後の端数ブロックも流す
static std::vector<float> runTone (KyoheiClipperProcessor& p, double f, float amp, int N)
{
    const double w = juce::MathConstants<double>::twoPi * f / 48000.0;
    std::vector<float> y ((size_t) N);
    juce::MidiBuffer m;
    for (int off = 0; off < N; off += 512)
    {
        const int len = juce::jmin (512, N - off);
        juce::AudioBuffer<float> b (2, len);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < len; ++i)
                b.setSample (c, i, (float) (amp * std::sin (w * (off + i))));
        p.processBlock (b, m);
        for (int i = 0; i < len; ++i)
            y[(size_t) (off + i)] = b.getSample (0, i);
    }
    return y;
}

// 定常区間 [N/4, N) の最大 |y|。非有限が混じっていたら finite を false にする
static float steadyPeak (const std::vector<float>& y, bool& finite)
{
    float pk = 0.0f;
    for (size_t n = y.size() / 4; n < y.size(); ++n)
    {
        if (! std::isfinite (y[n]))
            finite = false;
        else
            pk = std::max (pk, std::abs (y[n]));
    }
    return pk;
}

// 閾値のはるか下 (threshold 0 dB) で正弦を流し、定常区間の振幅比と
// 「位相から求めた遅延 − 報告レイテンシ」(サンプル) を返す。
// 窓 18000 サンプルは 100 Hz〜10 kHz のどれでも半周期の整数倍なので、lock-in の漏れが乗らない。
struct ToneResult { double gain, delayErr; bool finite; };
static ToneResult measureTone (float inGainDb, float outGainDb, double f, float amp)
{
    KyoheiClipperProcessor p;
    setParam (p, "threshold", 0.0f);
    setParam (p, "knee", 0.0f);
    setParam (p, "inputGain", inGainDb);
    setParam (p, "outputGain", outGainDb);
    p.prepareToPlay (48000.0, 512);
    const int D = p.getLatencySamples();
    const int N = 24000;
    const auto y = runTone (p, f, amp, N);

    const double w = juce::MathConstants<double>::twoPi * f / 48000.0;
    double I = 0.0, Q = 0.0;
    bool finite = true;
    for (int n = N / 4; n < N; ++n)
    {
        if (! std::isfinite (y[(size_t) n]))
            finite = false;
        I += y[(size_t) n] * std::sin (w * n);
        Q += y[(size_t) n] * std::cos (w * n);
    }
    const double A = 2.0 * std::sqrt (I * I + Q * Q) / (N - N / 4);
    // y ≈ A·sin(w(n − τ)) なので位相は −wτ。報告レイテンシ D との差を [−π, π) に畳んでサンプルに直す
    const double d = std::remainder (std::atan2 (Q, I) + w * D, juce::MathConstants<double>::twoPi);
    const double delayErr = -d / w;
    return { A / amp, delayErr, finite && std::isfinite (A) && A > 0.0 && std::isfinite (delayErr) };
}

static void testActivePathInvariants()
{
    printf ("T10: active-path audio against implementation-independent references\n");

    // I1 閾値のはるか下では、出力 = 入力を報告レイテンシだけ遅らせたもの (遅延も振幅も)。
    //    実遅延が報告とずれると、host の PDC とも soft bypass の dry とも合わなくなる (F2)
    double worstDelay = 0.0, worstGain = 0.0;
    bool finite = true;
    for (double f : { 100.0, 1000.0, 5000.0, 10000.0 })
    {
        const auto t = measureTone (0.0f, 0.0f, f, 0.1f);
        finite = finite && t.finite;
        if (t.finite)
        {
            worstDelay = std::max (worstDelay, std::abs (t.delayErr));
            worstGain  = std::max (worstGain,  std::abs (20.0 * std::log10 (t.gain)));
        }
    }
    printf ("     I1: |measured delay - reported latency| max %.4f samples, |gain| max %.4f dB\n",
            worstDelay, worstGain);
    check (finite && worstDelay < 0.01, "I1 below threshold, the output is delayed by exactly the reported latency");
    check (finite && worstGain < 0.1, "I1 below threshold, the level is unchanged");

    // I2 天井: B.Wall / knee 0 で +12 dB 突っ込んでも threshold に張り付く。
    //    Slammer は天井を保証しない設計 (ダウンサンプルのギブスで超える) なので、外れ検出用の幅にする
#if KYOHEI_SLAMMER
    constexpr float ceilingTolDb = 1.0f;
#else
    constexpr float ceilingTolDb = 0.2f;
#endif
    {
        KyoheiClipperProcessor p;
        setParam (p, "threshold", -6.0f);
        setParam (p, "knee", 0.0f);
        setParam (p, "inputGain", 12.0f);
        p.prepareToPlay (48000.0, 512);
        bool ok = true;
        const float pkDb = juce::Decibels::gainToDecibels (steadyPeak (runTone (p, 110.0, 0.9f, 96000), ok), -200.0f);
        printf ("     I2: output peak %+.3f dBFS at threshold -6.0 (+12 dB drive)\n", pkDb);
        check (ok && pkDb <= -6.0f + ceilingTolDb, "I2 driven hard, the output stays at the threshold");
    }

    // I3/I4 Output / Input ゲインが出音に届く (線形域なので +6 dB がそのまま +6 dB)
    {
        const auto t0   = measureTone (0.0f, 0.0f, 1000.0, 0.05f);
        const auto tOut = measureTone (0.0f, 6.0f, 1000.0, 0.05f);
        const auto tIn  = measureTone (6.0f, 0.0f, 1000.0, 0.05f);
        const bool ok = t0.finite && tOut.finite && tIn.finite;
        const double outDb = 20.0 * std::log10 (tOut.gain / t0.gain);
        const double inDb  = 20.0 * std::log10 (tIn.gain / t0.gain);
        printf ("     I3/I4: Output +6 dB -> %+.3f dB, Input +6 dB -> %+.3f dB\n", outDb, inDb);
        check (ok && std::abs (outDb - 6.0) < 0.05, "I3 the Output gain reaches the audio");
        check (ok && std::abs (inDb - 6.0) < 0.05, "I4 the Input gain reaches the audio");
    }

    // I5 ニー域: 閾値の 0.3 dB 下 (リミッターは動かない) の正弦を knee 12 dB で通すと、
    //    シェイパーだけが山を丸める。Clipper はリミッター単独でも天井を守るので、シェイパーが
    //    音から外れた欠陥は I2 では見えない。ここで見る
    {
        KyoheiClipperProcessor p;
        setParam (p, "threshold", -6.0f);
        setParam (p, "knee", 12.0f);
        p.prepareToPlay (48000.0, 512);
        const float amp = juce::Decibels::decibelsToGain (-6.3f);
        bool ok = true;
        const float pk = steadyPeak (runTone (p, 440.0, amp, 48000), ok);
        const float redDb = ok && pk > 0.0f ? 20.0f * std::log10 (amp / pk) : 0.0f;
        printf ("     I5: knee 12 dB, tone 0.3 dB under the threshold -> peak reduced by %.3f dB\n", redDb);
        check (ok && redDb > 0.3f, "I5 the shaper's knee reaches the audio");
    }
}

// F4: ホストの reset() (AU の Reset / VST3 の setProcessing(false)。AAX は呼ばない) の直後は、
// その時点の設定で新しく用意したインスタンスと**ビット一致**すること。v1.0.11 は reset() を
// 実装しておらず、直前の音 (Clipper 76 / Slammer 60 サンプル) と Clipper のリミッター減衰
// (−5.5 dB @1ms → −0.6 dB @100ms) が残っていた。bypass 中・bypass フェード途中の reset も見る。
static void testHostResetMatchesFreshInstance()
{
    printf ("T11: after the host's reset() the output equals a freshly prepared instance\n");

    const double fs = 48000.0;
    const int len = 128;
    const char* bypassNames[] = { "active", "bypassed", "mid-fade" };
    float worst = 0.0f;
    bool finite = true;
    int cases = 0;
    for (float mode : { 0.0f, 1.0f, 2.0f })
    {
        for (int bypassCase = 0; bypassCase < 3; ++bypassCase)
        {
            const auto setup = [mode] (KyoheiClipperProcessor& proc)
            {
                setParam (proc, "mode", mode);
                setParam (proc, "threshold", -12.0f);
                setParam (proc, "knee", 3.0f);
                setParam (proc, "inputGain", 12.0f);
            };
            KyoheiClipperProcessor p;
            setup (p);
            if (bypassCase == 1)
                setParam (p, "bypass", 1.0f);
            p.prepareToPlay (fs, len);
            Signal sig;
            sig.amp = 0.8f;
            runSegment (p, sig, 40 * len, { len }, false, nullptr, nullptr);   // 大音量で状態を濃くする
            if (bypassCase == 2)
            {
                setParam (p, "bypass", 1.0f);
                runSegment (p, sig, 2 * len, { len }, false, nullptr, nullptr); // 15ms フェードの途中
            }
            p.reset();

            KyoheiClipperProcessor q;
            setup (q);
            if (bypassCase != 0)
                setParam (q, "bypass", 1.0f);
            q.prepareToPlay (fs, len);

            Signal qs = sig;                        // 同じ位置から同じ入力
            const int window = p.getLatencySamples() + fadeSamples (fs) + 4096;
            std::vector<float> op, oq;
            runSegment (p, sig, window, { len }, false, nullptr, &op);
            runSegment (q, qs,  window, { len }, false, nullptr, &oq);
            float w = 0.0f;
            for (size_t n = 0; n < op.size(); ++n)
            {
                if (! std::isfinite (op[n]) || ! std::isfinite (oq[n]))
                    finite = false;
                else
                    w = std::max (w, std::abs (op[n] - oq[n]));
            }
            if (w != 0.0f)
                printf ("     mode %d / %s: max |after reset - fresh| %.3e\n",
                        (int) mode, bypassNames[bypassCase], w);
            worst = std::max (worst, w);
            ++cases;
        }
    }
    printf ("     worst |after reset() - fresh instance| = %.3e (%d cases)\n", worst, cases);
    check (finite && worst == 0.0f, "reset() leaves nothing a fresh instance would not have (all modes, bypass states)");
}

// F3: 長さ 0 の processBlock が Input/Output のゲインランプを使い切っていた。変更直後に
// 0 サンプル呼び出しが挟まると、次のブロックが旧ゲインから新ゲインへ段差で跳ぶ
// (実測 段差 0.097 / 通常のランプ 0.0004)。JUCE の VST3 ラッパーは、入出力バス付きの
// 0 サンプル process をそのまま processBlock に渡す。期待: 挟まないインスタンスとビット一致。
static void testZeroLengthBlockIsANoOp()
{
    printf ("T12: a zero-length block changes nothing\n");

    float worst = 0.0f;
    bool finite = true;
    for (float mode : { 0.0f, 2.0f })
    {
        KyoheiClipperProcessor p, q;
        for (auto* proc : { &p, &q })
        {
            setParam (*proc, "mode", mode);
            setParam (*proc, "threshold", -6.0f);
            proc->prepareToPlay (48000.0, 256);
        }
        Signal sp, sq;
        runSegment (p, sp, 20 * 256, { 256 }, false, nullptr, nullptr);
        runSegment (q, sq, 20 * 256, { 256 }, false, nullptr, nullptr);
        for (auto* proc : { &p, &q })
        {
            setParam (*proc, "inputGain", 6.0f);
            setParam (*proc, "outputGain", -3.0f);
        }
        juce::AudioBuffer<float> empty (2, 0);
        juce::MidiBuffer m;
        p.processBlock (empty, m);                   // p にだけ 0 サンプル呼び出しを挟む

        std::vector<float> op, oq;
        runSegment (p, sp, 8 * 256, { 256 }, false, nullptr, &op);
        runSegment (q, sq, 8 * 256, { 256 }, false, nullptr, &oq);
        for (size_t n = 0; n < op.size(); ++n)
        {
            if (! std::isfinite (op[n]) || ! std::isfinite (oq[n]))
                finite = false;
            else
                worst = std::max (worst, std::abs (op[n] - oq[n]));
        }
    }
    printf ("     worst |with a zero-length block - without| = %.3e\n", worst);
    check (finite && worst == 0.0f, "a zero-length block leaves the gain ramps (and everything else) untouched");
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    printf ("=== %s: processor integration ===\n", kProductName);
    testStateRoundTrip();
    testVisualiserFollowsBypass();
    testHardBypassResume();
    testMeterMatchesVisualiserDuringFade();
    testResumeUsesLatestSettings();
    testResumeHasNoClick();
    testResumeHoldIsSampleAccurate();
    testPrepareClearsResumeState();
    testAnalyserFrameDoesNotSpanHardBypass();
    testActivePathInvariants();
    testHostResetMatchesFreshInstance();
    testZeroLengthBlockIsANoOp();
    printf ("\n%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
