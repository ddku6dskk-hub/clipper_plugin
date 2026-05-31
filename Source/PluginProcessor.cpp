// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr float meterFloor = 1.0e-4f;
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
    // Clipper: 16x OS + lookahead で TDR 同等の透明さ
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
    const int lookaheadSamplesOS = juce::roundToInt (0.0002 * osSampleRate);
    for (auto& lim : limiters)
    {
        lim.prepare (osSampleRate, lookaheadSamplesOS,
                     50.0 /* release ms */,
                     0.05 /* stage-2 smooth ms */);
        lim.reset();
    }

    // 総レイテンシ: OS 群遅延 + limiter look-ahead (OS→base 換算)
    const double limiterBaseLatency = (double) lookaheadSamplesOS / std::pow (2.0, osFactor);
    const int reportedLatency = juce::roundToInt (oversampler->getLatencyInSamples() + limiterBaseLatency);
    setLatencySamples (reportedLatency);
#endif

    juce::dsp::ProcessSpec meterSpec;
    meterSpec.sampleRate = sampleRate;
    meterSpec.maximumBlockSize = (juce::uint32) samplesPerBlock;
    meterSpec.numChannels = 2;

    grMeterDelayLine.setMaximumDelayInSamples (juce::jmax (1, reportedLatency + 1));
    grMeterDelayLine.prepare (meterSpec);
    grMeterDelayLine.setDelay ((float) reportedLatency);
    grMeterDelayLine.reset();

    // soft-bypass: dry を wet と同じ reportedLatency だけ遅延して時間整合する専用ライン。
    // grMeterDelayLine とは別 (あちらは input gain 後、こちらは input gain 前の素入力)。
    dryDelayLine.setMaximumDelayInSamples (juce::jmax (1, reportedLatency + 1));
    dryDelayLine.prepare (meterSpec);                 // numChannels = 2
    dryDelayLine.setDelay ((float) reportedLatency);  // reportedLatency==0 でも setDelay(0) で退化
    dryDelayLine.reset();

    dryScratch.setSize (2, samplesPerBlock, false, false, true);
    dryScratch.clear();

    bypassMix.reset (sampleRate, 0.015); // 15ms ランプ
    bypassMix.setCurrentAndTargetValue (pBypass != nullptr && pBypass->load() > 0.5f ? 1.0f : 0.0f);

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

    // input gain
    buffer.applyGain (inGainLin);

    // 入力サンプルピーク (dBFS) は現在ブロックの値を UI に渡す。
    float peakIn = 0.0f;
    for (int c = 0; c < numChannels; ++c)
        peakIn = juce::jmax (peakIn, buffer.getMagnitude (c, 0, numSamples));

    if (! std::isfinite (peakIn))
        peakIn = 0.0f;

    inputPeakDb.store (juce::Decibels::gainToDecibels (peakIn, -100.0f), std::memory_order_relaxed);

    // GR 用 input peak は reported latency と同じ DelayLine で出力 peak と時間整合させる。
    float peakInForGr = 0.0f;
    for (int c = 0; c < numChannels; ++c)
    {
        const auto* src = buffer.getReadPointer (c);
        const int delayChannel = juce::jmin (c, 1);

        for (int i = 0; i < numSamples; ++i)
        {
            grMeterDelayLine.pushSample (delayChannel, src[i]);
            const float delayed = grMeterDelayLine.popSample (delayChannel);
            peakInForGr = juce::jmax (peakInForGr, std::abs (delayed));
        }
    }

    if (! std::isfinite (peakInForGr))
        peakInForGr = 0.0f;

    // oversample up
    juce::dsp::AudioBlock<float> block (buffer);
    auto osBlock = oversampler->processSamplesUp (block);

    const int osNumSamples = (int) osBlock.getNumSamples();
    for (int c = 0; c < numChannels; ++c)
    {
        auto* data = osBlock.getChannelPointer ((size_t) c);
        const size_t chIdx = (size_t) juce::jmin (c, 1);
        auto& chain   = chains  [chIdx];
#if KYOHEI_SLAMMER
        // Slammer: ドラム用途向けに shaper のみ（lookahead なし）でトランジェント保持
        for (int i = 0; i < osNumSamples; ++i)
            data[i] = chain.process (data[i]);
#else
        auto& limiter = limiters[chIdx];
        // 段1: look-ahead True-Peak limiter で大きな山を均す
        // 段2: soft clipper がすり抜けた速いトランジェントを仕上げ
        for (int i = 0; i < osNumSamples; ++i)
            data[i] = chain.process (limiter.process (data[i]));
#endif
    }

    // oversample down
    oversampler->processSamplesDown (block);

    // GR 計測用: クリップ後 peak (output gain 適用前)
    float peakOut = 0.0f;
    for (int c = 0; c < numChannels; ++c)
        peakOut = juce::jmax (peakOut, buffer.getMagnitude (c, 0, numSamples));

    if (! std::isfinite (peakOut))
        peakOut = 0.0f;

    // GR 計算 (peakInForGr > peakOut の場合のみ正の GR)
    float grDb = 0.0f;
    if (peakInForGr > meterFloor && peakOut > meterFloor && peakInForGr > peakOut)
        grDb = 20.0f * std::log10 (peakInForGr / peakOut);

    if (! std::isfinite (grDb))
        grDb = 0.0f;

    // output gain
    buffer.applyGain (outGainLin);

    // --- soft bypass crossfade: wet(buffer) ↔ 遅延 dry(dryScratch) ---
    // frame ごとに bypassMix を1回進めて全ch共通に適用。time-align 済みなのでクリックレス。
    // active 継続中(mix=0 かつ非平滑)はループを省いて従来どおり wet を素通し。
    float mixEnd = bypassMix.getCurrentValue();
    if (bypassMix.isSmoothing() || mixEnd > 0.0f)
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
    grPeakDb.store (grDb * (1.0f - mixEnd), std::memory_order_relaxed);

    // 出力 peak [dBFS] — クロスフェード後の最終信号レベル (クリップ LED 判定用)
    float peakFinal = 0.0f;
    for (int c = 0; c < numChannels; ++c)
        peakFinal = juce::jmax (peakFinal, buffer.getMagnitude (c, 0, numSamples));
    if (! std::isfinite (peakFinal))
        peakFinal = 0.0f;
    outputPeakDb.store (juce::Decibels::gainToDecibels (peakFinal, -100.0f),
                        std::memory_order_relaxed);
}

void KyoheiClipperProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer,
                                                    juce::MidiBuffer&)
{
    // getBypassParameter() を提供しているため、通常ホストはこの関数を呼ばず、host bypass は
    // processBlock 内の bypassMix クロスフェードで処理される。ただしフォーマット/ホスト差分の
    // 保険として、ここでも reportedLatency 分だけ信号を遅延させて出力し、active 時との
    // 時間ジャンプ (ポップ) を防ぐ。
    juce::ScopedNoDenormals denormals;

    const int numChannels = buffer.getNumChannels();
    const int numSamples  = buffer.getNumSamples();

    float peakIn = 0.0f;
    for (int c = 0; c < numChannels; ++c)
    {
        auto* d = buffer.getWritePointer (c);
        const int dch = juce::jmin (c, 1);
        for (int i = 0; i < numSamples; ++i)
        {
            dryDelayLine.pushSample (dch, d[i]);
            d[i] = dryDelayLine.popSample (dch);  // latency 整合済みの素通し
            peakIn = juce::jmax (peakIn, std::abs (d[i]));
        }
    }
    if (! std::isfinite (peakIn))
        peakIn = 0.0f;

    const float db = juce::Decibels::gainToDecibels (peakIn, -100.0f);
    inputPeakDb.store  (db,   std::memory_order_relaxed);
    grPeakDb.store     (0.0f, std::memory_order_relaxed); // bypass 中は GR なし
    outputPeakDb.store (db,   std::memory_order_relaxed);
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
