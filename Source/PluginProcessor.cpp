#include "PluginProcessor.h"
#include "PluginEditor.h"

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
    setLatencySamples (juce::roundToInt (oversampler->getLatencyInSamples()));
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
    setLatencySamples (juce::roundToInt (oversampler->getLatencyInSamples() + limiterBaseLatency));
#endif
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

    // input gain
    buffer.applyGain (inGainLin);

    // GR 計測用: クリップ前 peak (input gain 適用後)
    float peakIn = 0.0f;
    for (int c = 0; c < numChannels; ++c)
        peakIn = juce::jmax (peakIn, buffer.getMagnitude (c, 0, numSamples));

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

    // GR 計算 (peakIn > peakOut の場合のみ正の GR)
    float grDb = 0.0f;
    if (peakIn > 1.0e-6f && peakOut > 1.0e-6f && peakIn > peakOut)
        grDb = 20.0f * std::log10 (peakIn / peakOut);
    grPeakDb.store (grDb, std::memory_order_relaxed);

    // output gain
    buffer.applyGain (outGainLin);

    juce::ignoreUnused (numSamples);
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
