// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // UI メーター用 peak-hold: audio thread から複数ブロック分のピークを取りこぼさず
    // atomic に最大値として蓄積する。UI 側が読み取り時に exchange でリセットするので、
    // 「UI が読みに来るまでの全ブロックの最大」が常に表示される (旧: 毎ブロック上書きで
    // 最後のブロックしか見えず、PT メーターより低く出る取りこぼしがあった)。
    inline void atomicPeakMax (std::atomic<float>& target, float value) noexcept
    {
        float cur = target.load (std::memory_order_relaxed);
        while (value > cur
               && ! target.compare_exchange_weak (cur, value, std::memory_order_relaxed))
        {}
    }
}

KyoheiClipperProcessor::KyoheiClipperProcessor()
    : juce::AudioProcessor (BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "params", createLayout())
{
    pThreshold  = apvts.getRawParameterValue ("threshold");
    pKnee       = apvts.getRawParameterValue ("knee");
    pMode       = apvts.getRawParameterValue ("mode");
    pInputGain  = apvts.getRawParameterValue ("inputGain");
    pOutputGain = apvts.getRawParameterValue ("outputGain");
    pBypass     = apvts.getRawParameterValue ("bypass");

    // std::atomic<float> は既定構築では不定値なので、リングは必ず明示初期化する
    for (auto& chArr : visInDb)
        for (auto& f : chArr)
            f.store (-100.0f, std::memory_order_relaxed);
    for (auto& chArr : visGrDb)
        for (auto& f : chArr)
            f.store (0.0f, std::memory_order_relaxed);
}

juce::AudioProcessorParameter* KyoheiClipperProcessor::getBypassParameter() const
{
    return apvts.getParameter ("bypass");
}

juce::AudioProcessorValueTreeState::ParameterLayout
KyoheiClipperProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> params;

#if KYOHEI_SLAMMER
    // Slammer: 攻めた初期値。閾値浅め + ソフトニー中庸で頭の角を丸める
    const float defaultThreshold = -3.0f;
    const float defaultKnee      = 6.5f;
#else
    const float defaultThreshold = -6.0f;
    const float defaultKnee      = 3.0f;
#endif

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID {"threshold", 1}, "Threshold",
        NormalisableRange<float> (-18.0f, 0.0f, 0.1f), defaultThreshold,
        AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID {"knee", 1}, "Knee",
        NormalisableRange<float> (0.0f, 12.0f, 0.1f), defaultKnee,
        AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID {"mode", 1}, "Mode",
        StringArray { "B.Wall", "Open", "LF" }, 0));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID {"inputGain", 1}, "Input",
        NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID {"outputGain", 1}, "Output",
        NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));

    // 末尾に追加 (既存パラメータの順序/ID を変えず後方互換を維持)。
    // getBypassParameter() で host bypass に紐づく soft-bypass フラグ。
    params.push_back (std::make_unique<AudioParameterBool> (
        ParameterID {"bypass", 1}, "Bypass", false));

    return { params.begin(), params.end() };
}

void KyoheiClipperProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    preparedSampleRate = sampleRate;
#if KYOHEI_SLAMMER
    // Slammer: 4x OS、トランジェント重視、lookahead なし
    const int osFactor = 2; // 2^2 = 4x
#else
    // Clipper: 16x OS + lookahead で透明な音圧最大化を狙う
    const int osFactor = 4; // 2^4 = 16x
#endif
    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
        2, osFactor,
        juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
        true /* isMaxQuality */);

    oversampler->initProcessing ((size_t) samplesPerBlock);

    const double osSampleRate = sampleRate * std::pow (2.0, osFactor);
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = osSampleRate;
    spec.maximumBlockSize = (juce::uint32) (samplesPerBlock * (1 << osFactor));
    spec.numChannels = 1;

    for (auto& ch : chains)
    {
        ch.prepare (spec);
        ch.reset();
    }

#if KYOHEI_SLAMMER
    // Slammer: lookahead 段は使わない → OS 群遅延のみ
    const int reportedLatency = juce::roundToInt (oversampler->getLatencyInSamples());
    setLatencySamples (reportedLatency);
#else
    // look-ahead limiter: 0.2 ms 相当（OS レート基準）
    // スイープ実測 (推し活_M4 Open 6dB): 0.2ms が音圧最大スイートスポット (LUFS -7.24)
    // TP は +0.002dB の微超に収まる (可聴限界以下)。0.5ms 比 +0.04dB ホット
    //
    // さらに「OS 群遅延 + look-ahead」の合計がベースレートで整数サンプルになるよう
    // 0〜15 OS サンプル (≤0.02ms。スイープ感度 0.3ms 差で 0.04dB に対し無視できる量)
    // だけ切り上げる。これで報告レイテンシ = 実レイテンシが厳密一致し、host PDC と
    // soft-bypass の dry 整合からサブサンプル誤差 (クロスフェード中の微小コム) が消える。
    const double osFactorLin = std::pow (2.0, osFactor);
    const double osLatencyOS = (double) oversampler->getLatencyInSamples() * osFactorLin;
    int lookaheadSamplesOS   = (int) std::ceil (0.0002 * osSampleRate);
    const double fracOS = std::fmod (osLatencyOS + (double) lookaheadSamplesOS, osFactorLin);
    if (fracOS > 1e-6 && osFactorLin - fracOS > 1e-6)
        lookaheadSamplesOS += (int) std::llround (osFactorLin - fracOS);

    for (auto& lim : limiters)
    {
        lim.prepare (osSampleRate, lookaheadSamplesOS,
                     50.0 /* release ms */,
                     0.05 /* stage-2 smooth ms */);
        lim.reset();
    }

    // 総レイテンシ: OS 群遅延 + limiter look-ahead (OS→base 換算)。上記の切り上げにより
    // 通常は誤差ゼロの整数 (万一 OS 群遅延が OS サンプル非整数でも残差 < 1/16 サンプル)。
    const int reportedLatency = juce::roundToInt (
        (osLatencyOS + (double) lookaheadSamplesOS) / osFactorLin);
    setLatencySamples (reportedLatency);
#endif

    juce::dsp::ProcessSpec meterSpec;
    meterSpec.sampleRate = sampleRate;
    meterSpec.maximumBlockSize = (juce::uint32) samplesPerBlock;
    meterSpec.numChannels = 2;

    // soft-bypass: dry を wet と同じ reportedLatency だけ遅延して時間整合する専用ライン。
    dryDelayLine.setMaximumDelayInSamples (juce::jmax (1, reportedLatency + 1));
    dryDelayLine.prepare (meterSpec);                 // numChannels = 2
    dryDelayLine.setDelay ((float) reportedLatency);  // reportedLatency==0 でも setDelay(0) で退化
    dryDelayLine.reset();

    dryScratch.setSize (2, samplesPerBlock, false, false, true);
    dryScratch.clear();
    mixScratch.setSize (1, samplesPerBlock, false, false, true);
    mixScratch.clear();
    gainScratch.setSize (2, samplesPerBlock, false, false, true);
    gainScratch.clear();
    visInScratch.setSize (2, samplesPerBlock, false, false, true);
    visInScratch.clear();
    preparedBlockSize = samplesPerBlock;

    // ビジュアライザのフレーム長 (10ms)
    visFrameLen = juce::jmax (1, (int) std::llround (0.010 * sampleRate));
    visFrameCount = 0;
    visFramePeak.fill (0.0f);
    visFrameMinGain.fill (1.0f);
    for (auto& chArr : visInDb)
        for (auto& f : chArr)
            f.store (-100.0f, std::memory_order_relaxed);
    for (auto& chArr : visGrDb)
        for (auto& f : chArr)
            f.store (0.0f, std::memory_order_relaxed);
    visWritePos.store (0, std::memory_order_release);

    bypassMix.reset (sampleRate, 0.015); // 15ms ランプ
    bypassMix.setCurrentAndTargetValue (pBypass != nullptr && pBypass->load() > 0.5f ? 1.0f : 0.0f);

    // 再生開始時に古いゲインから不要なランプがかからないよう現在値で初期化
    lastInGain  = juce::Decibels::decibelsToGain (pInputGain  != nullptr ? pInputGain->load()  : 0.0f);
    lastOutGain = juce::Decibels::decibelsToGain (pOutputGain != nullptr ? pOutputGain->load() : 0.0f);

    // 上で wet 経路も bypassMix も作り直したので、hard bypass からの復帰待ちも捨てる。
    // 残すと prepare 後の再生が「dry 保持 → フェード」から始まり、新規インスタンスと食い違う。
    wetNeedsRestart = false;
    wetRestartHold  = 0;

    grPeakDb.store    (0.0f,    std::memory_order_relaxed);
    inputPeakDb.store (-100.0f, std::memory_order_relaxed);
    outputPeakDb.store (-100.0f, std::memory_order_relaxed);
    resetSessionPeaks();
}

bool KyoheiClipperProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& mainOut = layouts.getMainOutputChannelSet();
    if (mainOut != juce::AudioChannelSet::stereo() && mainOut != juce::AudioChannelSet::mono())
        return false;
    return layouts.getMainInputChannelSet() == mainOut;
}

void KyoheiClipperProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals denormals;

    // prepareToPlay 前に呼ぶ契約違反ホスト対策: 未初期化の oversampler/delay line に
    // 触らず素通しする (chunk 分割と同レベルの防御をここにも揃える)
    if (oversampler == nullptr || preparedBlockSize <= 0)
        return;

    // dryScratch/oversampler の確保量は prepareToPlay の samplesPerBlock 前提。
    // 超過ブロックを渡す契約違反ホストでもオーバーランしないよう、確保済みサイズ以下に
    // 分割して処理する (alloc なし: chunk は元バッファ参照のビュー)。通常ホストは1チャンク。
    const int totalSamples = buffer.getNumSamples();
    const int numChannels  = buffer.getNumChannels();
    const int maxChunk     = preparedBlockSize;

    if (totalSamples <= maxChunk)
    {
        processChunk (buffer);
        return;
    }

    for (int offset = 0; offset < totalSamples; offset += maxChunk)
    {
        const int len = juce::jmin (maxChunk, totalSamples - offset);
        juce::AudioBuffer<float> chunk (buffer.getArrayOfWritePointers(), numChannels, offset, len);
        processChunk (chunk);
    }
}

// Mode / Threshold / Knee を chain と limiter へ反映する。
// processChunk の冒頭と restartWetChain から呼ぶ。hard bypass 中は wet を回さないので、
// その間の設定変更は復帰時にここを通して入れる。
void KyoheiClipperProcessor::applyChainParams() noexcept
{
    const auto mode = static_cast<kyohei::dsp::ClipperChain<float>::Mode> ((int) pMode->load());
    const float threshDb = pThreshold->load();
    const float kneeDb   = pKnee->load();

    for (auto& ch : chains)
    {
        ch.setMode (mode);
        ch.setThresholdDb (threshDb);
        ch.setKneeDb (kneeDb);
    }
#if !KYOHEI_SLAMMER
    const float threshLin = juce::Decibels::decibelsToGain (threshDb);
    for (auto& lim : limiters)
        lim.setThreshold (threshLin);
#endif
}

void KyoheiClipperProcessor::processChunk (juce::AudioBuffer<float>& buffer)
{
    const float inGainLin  = juce::Decibels::decibelsToGain (pInputGain->load());
    const float outGainLin = juce::Decibels::decibelsToGain (pOutputGain->load());

    applyChainParams();

    const int numChannels = buffer.getNumChannels();
    const int numSamples  = buffer.getNumSamples();

    // 非有限サンプル (NaN/Inf) は入口で 0 に潰す。LinkedShelf の IIR は NaN が 1 サンプル
    // でも入るとフィードバック状態に残留し reset まで無音化するため (B.Wall はメモリレスで
    // 自然復帰するが Open/LF は復帰しない)。メーター側 isfinite ガードと対になる音声側ガード。
    for (int c = 0; c < numChannels; ++c)
    {
        auto* d = buffer.getWritePointer (c);
        for (int i = 0; i < numSamples; ++i)
            if (! std::isfinite (d[i]))
                d[i] = 0.0f;
    }

    // --- hard bypass からの復帰 ---
    // 作り直した直後の wet は空なので、レイテンシ分は dry のまま出し、そのあと
    // 15ms かけて wet へ渡す。いきなり切り替えると、非ゼロの dry から空の wet へ
    // 直結されてクリックになる。soft bypass のクロスフェード機構をそのまま使う。
    // 保持の残りはサンプル単位で数え、チャンクの途中でもそこからフェードを始める
    // (下の mix 曲線を作る箇所)。チャンク単位で減らすと、保持がブロック長に切り上がり
    // 512 で 26ms、2048 で 58ms と復帰がブロック長しだいで伸びていた。
    if (wetNeedsRestart)
    {
        restartWetChain();
        bypassMix.setCurrentAndTargetValue (1.0f);      // いったん全 dry
        wetRestartHold = getLatencySamples();           // wet が満ちるまで dry を保持
        wetNeedsRestart = false;
    }

    const float bypassTarget = pBypass != nullptr && pBypass->load() > 0.5f ? 1.0f : 0.0f;
    if (wetRestartHold <= 0)
        bypassMix.setTargetValue (bypassTarget);

    // --- soft bypass: 素入力(input gain 前)を dry として確保し、wet と同じ reportedLatency
    //     だけ遅延させて時間整合する。hard bypass 中も processBlockBypassed が同じ遅延線を回す。 ---
    for (int c = 0; c < numChannels; ++c)
    {
        const auto* src = buffer.getReadPointer (c);
        auto* dst = dryScratch.getWritePointer (c);
        const int dch = juce::jmin (c, 1);
        for (int i = 0; i < numSamples; ++i)
        {
            dryDelayLine.pushSample (dch, src[i]);
            dst[i] = dryDelayLine.popSample (dch);
        }
    }

    // input gain — 前ブロック値から線形ランプ (ブロック境界の段差 = ジッパーノイズ防止)
    buffer.applyGainRamp (0, numSamples, lastInGain, inGainLin);
    lastInGain = inGainLin;

    // 入力サンプルピーク (dBFS)。peak-hold で UI が読み取るまでの全ブロック最大を保持。
    float peakIn = 0.0f;
    for (int c = 0; c < numChannels; ++c)
        peakIn = juce::jmax (peakIn, buffer.getMagnitude (c, 0, numSamples));

    if (! std::isfinite (peakIn))
        peakIn = 0.0f;

    const float peakInDb = juce::Decibels::gainToDecibels (peakIn, -100.0f);
    atomicPeakMax (inputPeakDb, peakInDb);
    atomicPeakMax (sessionPeakDb, peakInDb);

    // ビジュアライザの入力レーン用に |入力| を控えておく (OS 後の buffer は処理済み信号に
    // 変わってしまうため、input gain 適用直後のここでしか取れない)
    for (int c = 0; c < juce::jmin (numChannels, 2); ++c)
    {
        const auto* src = buffer.getReadPointer (c);
        auto* dst = visInScratch.getWritePointer (c);
        for (int i = 0; i < numSamples; ++i)
            dst[i] = std::abs (src[i]);
    }

    // oversample up
    juce::dsp::AudioBlock<float> block (buffer);
    auto osBlock = oversampler->processSamplesUp (block);

    const int osNumSamples = (int) osBlock.getNumSamples();

    // GR は「各段が実際に適用したゲイン係数」を OS ドメイン内で直接測る (= 大手と同方式)。
    // up/down サンプリングの FIR リンギングや位相回しはこのループの外側なので構造的に混入せず、
    // 閾値以下の素通し区間では適用ゲイン == 1 (= 0 dB) になる。
    //
    // osPeak: 処理後の OS ドメインピーク。ダウンサンプル前の帯域制限済み波形の最大値なので、
    // ベースレート sample peak が取りこぼす inter-sample peak を含む True-Peak 推定になる
    // (クリップ LED 判定用。ダウンサンプル FIR のリンギング超過 +0.002dB 実測のみ漏れる)。
    // 適用ゲインは base サンプル単位へ畳んで gainScratch に置く。OS の R サンプルを
    // 1 base サンプルに min で集約する (GR は最小ゲイン = 最大リダクションで代表させる)。
    // GR メーターとビジュアライザは、どちらもこの gainScratch に bypass の mix を
    // サンプルごとに掛けた実効ゲインから出す (下の「実効ゲイン」参照)。
    const int osRatio = juce::jmax (1, osNumSamples / juce::jmax (1, numSamples));

    float osPeak = 0.0f;
    for (int c = 0; c < numChannels; ++c)
    {
        auto* data = osBlock.getChannelPointer ((size_t) c);
        const size_t chIdx = (size_t) juce::jmin (c, 1);
        auto& chain   = chains  [chIdx];

        auto* gainOut = gainScratch.getWritePointer ((int) chIdx);
        float groupMin = 1.0f;      // 今の base サンプルへ畳み込み中の最小ゲイン
        int baseIdx = 0, groupCount = 0;

        // 除算を内側ループに入れないためカウンタで境界を判定する (16x のホットループ)
        const auto accumulate = [&] (float gain) noexcept
        {
            groupMin = juce::jmin (groupMin, gain);
            if (++groupCount >= osRatio)
            {
                if (baseIdx < numSamples)
                    gainOut[baseIdx] = groupMin;
                ++baseIdx;
                groupCount = 0;
                groupMin = 1.0f;
            }
        };

#if KYOHEI_SLAMMER
        // Slammer: ドラム用途向けに shaper のみ（lookahead なし）でトランジェント保持
        for (int i = 0; i < osNumSamples; ++i)
        {
            data[i] = chain.process (data[i]);
            accumulate (chain.getLastShaperGain());
            osPeak = juce::jmax (osPeak, std::abs (data[i]));
        }
#else
        auto& limiter = limiters[chIdx];
        // 段1: look-ahead True-Peak limiter で大きな山を均す
        // 段2: soft clipper がすり抜けた速いトランジェントを仕上げ
        for (int i = 0; i < osNumSamples; ++i)
        {
            const float limited = limiter.process (data[i]);
            data[i] = chain.process (limited);
            // 総 GR = limiter 適用ゲイン × shaper 適用ゲイン (どちらも出力サンプルに整合済み)
            accumulate (limiter.getLastGain() * chain.getLastShaperGain());
            osPeak = juce::jmax (osPeak, std::abs (data[i]));
        }
#endif

        // 端数 (osNumSamples が numSamples の整数倍でない契約違反ケース) の穴埋め
        for (int n = baseIdx; n < numSamples; ++n)
            gainOut[n] = groupMin;
    }

    // oversample down
    oversampler->processSamplesDown (block);

    // --- soft bypass の per-sample mix 係数を先に確定させる ---
    // ビジュアライザの GR レーンは「実際に出力へ効いている減衰量」でなければならないので、
    // フレーム蓄積より前に bypass 量を知っておく。完全 bypass 中に琥珀 (削られた分) が
    // 出続けるのを防ぐ (GR メーターは既に bypass 補正済みで、表示同士が食い違っていた)。
    auto* mixCurve = mixScratch.getWritePointer (0);
    const float mixStart = bypassMix.getCurrentValue();          // ランプを進める前の値
    const bool bypassMixing = bypassMix.isSmoothing() || mixStart > 0.0f;   // 復帰の保持中は mixStart == 1
    if (bypassMixing)
    {
        int i = 0;
        if (wetRestartHold > 0)
        {
            // hard bypass 復帰の dry 保持。残りを使い切ったサンプルからフェードを始める
            const int held = juce::jmin (wetRestartHold, numSamples);
            for (; i < held; ++i)
                mixCurve[i] = 1.0f;
            wetRestartHold -= held;
            if (wetRestartHold == 0)
                bypassMix.setTargetValue (bypassTarget);
        }
        for (; i < numSamples; ++i)
            mixCurve[i] = bypassMix.getNextValue();
    }
    else
    {
        juce::FloatVectorOperations::clear (mixCurve, numSamples);
    }
    const float mixEnd = bypassMix.getCurrentValue();

    // --- ビジュアライザ: 10ms フレームに畳んでリングへ (K Peak Controller と同設計) ---
    // 入力レーン = input gain 適用後の |入力| (visInScratch)、GR レーン = 実際に適用した
    // ゲイン (gainScratch)。両方とも同じ base サンプル index なので時間が揃っている。
    // blockMinEffGain: 同じ実効ゲインのブロック内最小。GR メーターはこれを使う。
    float blockMinEffGain = 1.0f;
    if (numChannels > 0)
    {
        const int lanes = juce::jlimit (1, 2, numChannels);
        visNumChannels.store (lanes, std::memory_order_relaxed);

        for (int i = 0; i < numSamples; ++i)
        {
            // 実効ゲイン: 出力 = wet·(1−m) + dry·m = dry·(g·(1−m) + m)。
            // bypass 中は g_eff = 1 になるので、削られていない波形が正しく描かれる。
            const float m = mixCurve[i];
            const float wet = 1.0f - m;
            for (int c = 0; c < lanes; ++c)
            {
                const float effGain = gainScratch.getReadPointer (c)[i] * wet + m;
                blockMinEffGain = juce::jmin (blockMinEffGain, effGain);
                visFramePeak[(size_t) c] =
                    juce::jmax (visFramePeak[(size_t) c], visInScratch.getReadPointer (c)[i]);
                visFrameMinGain[(size_t) c] = juce::jmin (visFrameMinGain[(size_t) c], effGain);
            }

            if (++visFrameCount >= visFrameLen)
            {
                const int w = visWritePos.load (std::memory_order_relaxed);
                for (int c = 0; c < lanes; ++c)
                {
                    visInDb[(size_t) c][(size_t) w].store (
                        juce::Decibels::gainToDecibels (visFramePeak[(size_t) c], -100.0f),
                        std::memory_order_relaxed);
                    const float mg = visFrameMinGain[(size_t) c];
                    const float fGr = (mg > 0.0f && mg < 1.0f) ? -20.0f * std::log10 (mg) : 0.0f;
                    visGrDb[(size_t) c][(size_t) w].store (std::isfinite (fGr) ? fGr : 0.0f,
                                                           std::memory_order_relaxed);
                }
                // release: UI (acquire 読み) が新 writePos を見た時点で当該フレーム値の可視を保証
                visWritePos.store ((w + 1) % kVisFrames, std::memory_order_release);
                visFrameCount = 0;
                visFramePeak.fill (0.0f);
                visFrameMinGain.fill (1.0f);
            }
        }
    }

    // output gain — input 側と同様にランプ適用
    const float outGainStart = lastOutGain;
    buffer.applyGainRamp (0, numSamples, lastOutGain, outGainLin);
    lastOutGain = outGainLin;

    // --- soft bypass crossfade: wet(buffer) ↔ 遅延 dry(dryScratch) ---
    // frame ごとに bypassMix を1回進めて全ch共通に適用。time-align 済みなのでクリックレス。
    // active 継続中(mix=0 かつ非平滑)はループを省いて従来どおり wet を素通し。
    if (bypassMixing)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float m = mixCurve[i];
            const float wetGain = 1.0f - m;
            for (int c = 0; c < numChannels; ++c)
            {
                auto* w = buffer.getWritePointer (c);
                w[i] = w[i] * wetGain + dryScratch.getReadPointer (c)[i] * m;
            }
        }
    }

    // GR メーターは bypass 量に応じてフェード (全 bypass で 0 表示)。
    // ビジュアライザと**同じサンプルごとの実効ゲイン** g_eff = g·(1−m) + m の最小から出す。
    // 【重要】ずれる書き方が 2 つあった:
    //  - dB 値に wet 比率を掛ける → 式が違う (g=0.1・m=0.5 で 5.19dB 対 10dB)
    //  - ブロック最小ゲインにブロック末尾の mix を掛ける → 集計区間が違う
    //    (フェード開始ブロックで 3.99dB 対 1.14dB。最大 GR の瞬間の mix ではないため)
    const float grEffDb = (blockMinEffGain > 0.0f && blockMinEffGain < 1.0f)
                              ? -20.0f * std::log10 (blockMinEffGain) : 0.0f;
    atomicPeakMax (grPeakDb, grEffDb);
    atomicPeakMax (sessionGrDb, grEffDb);   // 情報行 "Max GR" (実測の最大)

    // 出力 peak [dBFS] — クロスフェード後の最終信号レベル (クリップ LED 判定用)。
    // ベースレート sample peak に加え、OS ドメイン処理後ピーク × 出力ゲイン × wet 比率の
    // True-Peak 推定を合成して判定する (ダウンサンプルで生じる ISP の取りこぼし防止)。
    // ゲイン/ミックスはブロック内で変動しうるため保守側 (大きいゲイン・wet 多い方) を採用。
    // 全 bypass 中 (wet 比率 0) は従来どおり dry の sample peak にフォールバックする。
    float peakFinal = 0.0f;
    for (int c = 0; c < numChannels; ++c)
        peakFinal = juce::jmax (peakFinal, buffer.getMagnitude (c, 0, numSamples));
    const float wetTp = osPeak * juce::jmax (outGainStart, outGainLin)
                               * (1.0f - juce::jmin (mixStart, mixEnd));
    peakFinal = juce::jmax (peakFinal, wetTp);
    if (! std::isfinite (peakFinal))
        peakFinal = 0.0f;
    atomicPeakMax (outputPeakDb, juce::Decibels::gainToDecibels (peakFinal, -100.0f));
}

// hard bypass から戻ったときに wet 経路を作り直す。
//
// 【なぜウォームし続けないのか】bypass 中も毎ブロック wet を回すと、状態は途切れないが
// **bypass が active の 0.89 倍のCPUを食い続ける** (実測: active 7.80% / bypass 6.97% /
// 回さない場合 0.09%。1インスタンス・48kHz・512サンプル)。「重いプラグインを bypass して
// CPU を浮かせる」運用が成立しなくなるので、回すのをやめて復帰時に作り直す。
//
// 【設定は reset の前と後の両方で入れる】limiter と chain で reset の性質が逆だから。
//  - LookaheadLimiter::reset() は threshInit を残し、threshold を targetThreshold へ
//    スナップする → reset の**前**に目標値が最新でないと、旧値へスナップする。
//  - ClipperChain::reset() は threshInit/kneeInit を下ろすだけで、平滑化の現在値と
//    進行中のランプは残す → reset の**後**に設定を入れ直さないと、復帰の最初のチャンクが
//    旧 threshold/knee からランプする (2026-09-11 R1。旧コメントの「順序はどちらでも同じ」は
//    chain については誤りだった)。後の呼び出しで初回フラグが立ち、最新値へ即スナップする。
// 2 回目の applyChainParams() は limiter には何もしない (同値の setThreshold は早期 return)。
void KyoheiClipperProcessor::restartWetChain() noexcept
{
    applyChainParams();

    if (oversampler != nullptr)
        oversampler->reset();
    for (auto& ch : chains)
        ch.reset();
#if !KYOHEI_SLAMMER
    for (auto& lim : limiters)
        lim.reset();
#endif

    applyChainParams();
}

void KyoheiClipperProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer,
                                                    juce::MidiBuffer&)
{
    // getBypassParameter() を提供しているため、通常ホストはこの関数を呼ばず、host bypass は
    // processBlock 内の bypassMix クロスフェードで処理される。ただしフォーマット/ホスト差分の
    // 保険として、ここでも reportedLatency 分だけ信号を遅延させて出力し、active 時との
    // 時間ジャンプ (ポップ) を防ぐ。
    juce::ScopedNoDenormals denormals;

    // prepareToPlay 前は dryDelayLine が未確保のため触らない (processBlock と同じ防御)
    if (preparedBlockSize <= 0)
        return;

    const int numChannels  = buffer.getNumChannels();
    const int totalSamples = buffer.getNumSamples();
    if (numChannels <= 0 || totalSamples <= 0)
        return;

    float peakIn = 0.0f;

    // 確保済みサイズ以下に分割 (oversampler/dryScratch は preparedBlockSize 前提)
    for (int offset = 0; offset < totalSamples; offset += preparedBlockSize)
    {
        const int len = juce::jmin (preparedBlockSize, totalSamples - offset);

        for (int c = 0; c < numChannels; ++c)
        {
            auto* d = buffer.getWritePointer (c) + offset;
            const int dch = juce::jmin (c, 1);
            for (int i = 0; i < len; ++i)
            {
                float v = d[i];
                if (! std::isfinite (v))       // NaN/Inf は遅延線に入れない (processChunk と同基準)
                    v = 0.0f;
                dryDelayLine.pushSample (dch, v);
                d[i] = dryDelayLine.popSample (dch);  // latency 整合済みの素通し
                peakIn = juce::jmax (peakIn, std::abs (d[i]));
            }
        }

    }

    // wet は回さない (CPU を食わないのが hard bypass の存在意義)。
    // 代わりに「次に processBlock へ戻ったら作り直す」印を立てる。凍結したまま復帰すると
    // oversampler/limiter/chain に残った **bypass 直前の音** がそのまま出る
    // (実測: DC 0.1 を処理 → 無音を 20 ブロック bypass → 復帰1ブロック目で
    //  無音入力なのに出力ピーク 0.0999)。
    wetNeedsRestart = true;

    // 書きかけのビジュアライザフレームは捨てる。残すと、復帰後に続きを足して公開したとき
    // bypass 前の GR・入力レベルが混ざる (実測: 無音で復帰したのに 16dB の GR フレーム)。
    // 完成済みのフレームと情報行のセッション最大は履歴なので残す (bypass 中はグラフが止まる仕様)。
    visFrameCount = 0;
    visFramePeak.fill (0.0f);
    visFrameMinGain.fill (1.0f);

    if (! std::isfinite (peakIn))
        peakIn = 0.0f;

    const float db = juce::Decibels::gainToDecibels (peakIn, -100.0f);
    atomicPeakMax (inputPeakDb, db);
    atomicPeakMax (sessionPeakDb, db);                    // 情報行 "Peak" は素通し中も追う
    grPeakDb.store (0.0f, std::memory_order_relaxed);     // bypass 中は GR なし
    atomicPeakMax (outputPeakDb, db);

    // hard bypass 中も gain 履歴をパラメータに追従させ、active 復帰ブロックで
    // 古い値からの catch-up ランプが走らないようにする
    lastInGain  = juce::Decibels::decibelsToGain (pInputGain  != nullptr ? pInputGain->load()  : 0.0f);
    lastOutGain = juce::Decibels::decibelsToGain (pOutputGain != nullptr ? pOutputGain->load() : 0.0f);
}

juce::AudioProcessorEditor* KyoheiClipperProcessor::createEditor()
{
    return new KyoheiClipperEditor (*this);
}

void KyoheiClipperProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // apvts.state を直接書き出すと、パラメータ変更が ValueTree へ反映される前
    // (APVTS 内部のフラッシュ待ち) の古い値が保存されることがある
    // (実測: Threshold を -12dB にした直後の保存で Clipper は -6、Slammer は -3 が入る)。
    // copyState() は保留中の変更を反映してからスナップショットを返すので取りこぼさない。
    // 呼び出しスレッドはホスト依存 (JUCE ラッパーはホストの保存要求から直接呼ぶ)。
    // copyState() も setStateInformation 側の replaceState() もロックを取るので、
    // **audio thread からは呼ばないこと**。
    juce::MemoryOutputStream mos (destData, false);
    apvts.copyState().writeToStream (mos);
}

void KyoheiClipperProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto tree = juce::ValueTree::readFromData (data, (size_t) sizeInBytes);
    if (tree.isValid())
        apvts.replaceState (tree);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KyoheiClipperProcessor();
}
