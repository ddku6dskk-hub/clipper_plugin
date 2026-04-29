// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class KyoheiClipperEditor final : public juce::AudioProcessorEditor,
                                  private juce::Timer
{
public:
    explicit KyoheiClipperEditor (KyoheiClipperProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void drawGrMeter (juce::Graphics&, juce::Rectangle<int> bounds);

    using APVTS = juce::AudioProcessorValueTreeState;
    using SliderAttach = APVTS::SliderAttachment;
    using ChoiceAttach = APVTS::ComboBoxAttachment;

    KyoheiClipperProcessor& proc;

    juce::Slider thresholdSlider, kneeSlider, inputGainSlider, outputGainSlider;
    juce::ComboBox modeBox;
    juce::Label thresholdLabel, kneeLabel, inputLabel, outputLabel, modeLabel, grLabel;

    std::unique_ptr<SliderAttach> thresholdAtt, kneeAtt, inAtt, outAtt;
    std::unique_ptr<ChoiceAttach> modeAtt;

    // GR メーター状態 (Timer から更新)
    float grSmoothed = 0.0f;   // 表示用に 1次LP smoothed value
    float grPeakHold = 0.0f;   // ピークホールド値
    int   peakHoldFramesLeft = 0;

    // 入力サンプルピーク状態 (dBFS、Timer から更新)
    float inputPeakSmoothed     = -100.0f;
    float inputPeakHold         = -100.0f;
    int   inputPeakHoldFramesLeft = 0;

    juce::Rectangle<int> grMeterBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KyoheiClipperEditor)
};
