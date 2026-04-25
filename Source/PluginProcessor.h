// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "dsp/ClipperChain.h"
#include "dsp/LookaheadLimiter.h"

class KyoheiClipperProcessor final : public juce::AudioProcessor
{
public:
    KyoheiClipperProcessor();
    ~KyoheiClipperProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override
    {
#if KYOHEI_SLAMMER
        return "K Slammer";
#else
        return "K Clipper";
#endif
    }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // GR (Gain Reduction) 量 [dB] — input gain 適用後 peak と output gain 適用前 peak の差
    // UI 側で Timer から読み取って描画する
    std::atomic<float> grPeakDb { 0.0f };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    // 16x oversampling + OS 内 lookahead limiter + soft clipper のハイブリッド
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    std::array<kyohei::dsp::LookaheadLimiter<float>, 2> limiters; // stereo L/R
    std::array<kyohei::dsp::ClipperChain<float>, 2> chains;       // stereo L/R

    std::atomic<float>* pThreshold = nullptr;
    std::atomic<float>* pKnee = nullptr;
    std::atomic<float>* pMode = nullptr;
    std::atomic<float>* pInputGain = nullptr;
    std::atomic<float>* pOutputGain = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KyoheiClipperProcessor)
};
