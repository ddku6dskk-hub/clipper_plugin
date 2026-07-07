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
    preparedBlockSize = samplesPerBlock;

    bypassMix.reset (sampleRate, 0.015); // 15ms ランプ
    bypassMix.setCurrentAndTargetValue (pBypass != nullptr && pBypass->load() > 0.5f ? 1.0f : 0.0f);

    // 再生開始時に古いゲインから不要なランプがかからないよう現在値で初期化
    lastInGain  = juce::Decibels::decibelsToGain (pInputGain  != nullptr ? pInputGain->load()  : 0.0f);
    lastOutGain = juce::Decibels::decibelsToGain (pOutputGain != nullptr ? pOutputGain->load() : 0.0f);

    grPeakDb.store    (0.0f,    std::memory_order_relaxed);
    inputPeakDb.store (-100.0f, std::memory_order_relaxed);
    outputPeakDb.store (-100.0f, std::memory_order_relaxed);
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

void KyoheiClipperProcessor::processChunk (juce::AudioBuffer<float>& buffer)
{
    const auto mode = static_cast<kyohei::dsp::ClipperChain<float>::Mode> ((int) pMode->load());
    const float threshDb = pThreshold->load();
    const float kneeDb   = pKnee->load();
    const float inGainLin  = juce::Decibels::decibelsToGain (pInputGain->load());
    const float outGainLin = juce::Decibels::decibelsToGain (pOutputGain->load());

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

    // --- soft bypass: 素入力(input gain 前)を dry として確保し、wet と同じ reportedLatency
    //     だけ遅延させて時間整合する。bypass 状態に関わらず毎ブロック回してウォーム維持。 ---
    bypassMix.setTargetValue (pBypass != nullptr && pBypass->load() > 0.5f ? 1.0f : 0.0f);
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

    atomicPeakMax (inputPeakDb, juce::Decibels::gainToDecibels (peakIn, -100.0f));

    // oversample up
    juce::dsp::AudioBlock<float> block (buffer);
    auto osBlock = oversampler->processSamplesUp (block);

    const int osNumSamples = (int) osBlock.getNumSamples();

    // GR は「各段が実際に適用したゲイン係数」を OS ドメイン内で直接測る (= 大手と同方式)。
    // up/down サンプリングの FIR リンギングや位相回しはこのループの外側なので構造的に混入せず、
    // 閾値以下の素通し区間では blockMinGain == 1 (= 0 dB) になる。
    //
    // osPeak: 処理後の OS ドメインピーク。ダウンサンプル前の帯域制限済み波形の最大値なので、
    // ベースレート sample peak が取りこぼす inter-sample peak を含む True-Peak 推定になる
    // (クリップ LED 判定用。ダウンサンプル FIR のリンギング超過 +0.002dB 実測のみ漏れる)。
    float blockMinGain = 1.0f;
    float osPeak = 0.0f;
    for (int c = 0; c < numChannels; ++c)
    {
        auto* data = osBlock.getChannelPointer ((size_t) c);
        const size_t chIdx = (size_t) juce::jmin (c, 1);
        auto& chain   = chains  [chIdx];
#if KYOHEI_SLAMMER
        // Slammer: ドラム用途向けに shaper のみ（lookahead なし）でトランジェント保持
        for (int i = 0; i < osNumSamples; ++i)
        {
            data[i] = chain.process (data[i]);
            blockMinGain = juce::jmin (blockMinGain, chain.getLastShaperGain());
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
            blockMinGain = juce::jmin (blockMinGain,
                                       limiter.getLastGain() * chain.getLastShaperGain());
            osPeak = juce::jmax (osPeak, std::abs (data[i]));
        }
#endif
    }

    // oversample down
    oversampler->processSamplesDown (block);

    // GR [dB] = 潰した量。blockMinGain ∈ (0,1] を dB 化 (素通し時は 0 dB)。
    float grDb = 0.0f;
    if (blockMinGain > 0.0f && blockMinGain < 1.0f)
        grDb = -20.0f * std::log10 (blockMinGain);

    if (! std::isfinite (grDb))
        grDb = 0.0f;

    // output gain — input 側と同様にランプ適用
    const float outGainStart = lastOutGain;
    buffer.applyGainRamp (0, numSamples, lastOutGain, outGainLin);
    lastOutGain = outGainLin;

    // --- soft bypass crossfade: wet(buffer) ↔ 遅延 dry(dryScratch) ---
    // frame ごとに bypassMix を1回進めて全ch共通に適用。time-align 済みなのでクリックレス。
    // active 継続中(mix=0 かつ非平滑)はループを省いて従来どおり wet を素通し。
    const float mixStart = bypassMix.getCurrentValue();
    float mixEnd = mixStart;
    if (bypassMix.isSmoothing() || mixStart > 0.0f)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float m = bypassMix.getNextValue();
            const float wetGain = 1.0f - m;
            for (int c = 0; c < numChannels; ++c)
            {
                auto* w = buffer.getWritePointer (c);
                w[i] = w[i] * wetGain + dryScratch.getReadPointer (c)[i] * m;
            }
        }
        mixEnd = bypassMix.getCurrentValue();
    }

    // GR メーターは bypass 量に応じてフェード (全 bypass で 0 表示)
    atomicPeakMax (grPeakDb, grDb * (1.0f - mixEnd));

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

    const int numChannels = buffer.getNumChannels();
    const int numSamples  = buffer.getNumSamples();

    float peakIn = 0.0f;
    for (int c = 0; c < numChannels; ++c)
    {
        auto* d = buffer.getWritePointer (c);
        const int dch = juce::jmin (c, 1);
        for (int i = 0; i < numSamples; ++i)
        {
            float v = d[i];
            if (! std::isfinite (v))       // NaN/Inf は遅延線に入れない (processChunk と同基準)
                v = 0.0f;
            dryDelayLine.pushSample (dch, v);
            d[i] = dryDelayLine.popSample (dch);  // latency 整合済みの素通し
            peakIn = juce::jmax (peakIn, std::abs (d[i]));
        }
    }
    if (! std::isfinite (peakIn))
        peakIn = 0.0f;

    const float db = juce::Decibels::gainToDecibels (peakIn, -100.0f);
    atomicPeakMax (inputPeakDb, db);
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
    juce::MemoryOutputStream mos (destData, false);
    apvts.state.writeToStream (mos);
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
