#include "PluginEditor.h"

namespace
{
    void configureVertical (juce::Slider& s)
    {
        s.setSliderStyle (juce::Slider::LinearVertical);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 20);
    }
    void configureLabel (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centred);
    }
}

KyoheiClipperEditor::KyoheiClipperEditor (KyoheiClipperProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p)
{
    setSize (700, 340);

    for (auto* s : { &thresholdSlider, &kneeSlider, &inputGainSlider, &outputGainSlider })
    {
        configureVertical (*s);
        addAndMakeVisible (*s);
    }

    addAndMakeVisible (modeBox);
    modeBox.addItemList ({ "B.Wall", "Open", "LF" }, 1);

    configureLabel (thresholdLabel, "Threshold");
    configureLabel (kneeLabel,      "Knee");
    configureLabel (inputLabel,     "Input");
    configureLabel (outputLabel,    "Output");
    configureLabel (modeLabel,      "Mode");
    configureLabel (grLabel,        "GR");
    for (auto* l : { &thresholdLabel, &kneeLabel, &inputLabel, &outputLabel, &modeLabel, &grLabel })
        addAndMakeVisible (*l);

    thresholdAtt = std::make_unique<SliderAttach> (proc.apvts, "threshold",  thresholdSlider);
    kneeAtt      = std::make_unique<SliderAttach> (proc.apvts, "knee",       kneeSlider);
    inAtt        = std::make_unique<SliderAttach> (proc.apvts, "inputGain",  inputGainSlider);
    outAtt       = std::make_unique<SliderAttach> (proc.apvts, "outputGain", outputGainSlider);
    modeAtt      = std::make_unique<ChoiceAttach> (proc.apvts, "mode",       modeBox);

    startTimerHz (30);
}

void KyoheiClipperEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (28, 28, 32));
    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
#if KYOHEI_SLAMMER
    const juce::String title { "K Slammer" };
#else
    const juce::String title { "K Clipper" };
#endif
    g.drawText (title, getLocalBounds().removeFromTop (28), juce::Justification::centred);

    drawGrMeter (g, grMeterBounds);
}

void KyoheiClipperEditor::resized()
{
    auto area = getLocalBounds().reduced (10);
    area.removeFromTop (30); // title
    const int cellW = 130, labelH = 20, sliderH = 210;

    // 4 列セル + GR メーター
    auto row = area.removeFromTop (labelH + sliderH + 10);
    auto placeCell = [&] (juce::Label& label, juce::Slider& slider)
    {
        auto cell = row.removeFromLeft (cellW);
        label.setBounds  (cell.removeFromTop (labelH));
        slider.setBounds (cell.withSizeKeepingCentre (60, sliderH));
    };
    placeCell (thresholdLabel, thresholdSlider);
    placeCell (kneeLabel,      kneeSlider);
    placeCell (inputLabel,     inputGainSlider);
    placeCell (outputLabel,    outputGainSlider);

    // GR メーター: ラベル + 縦バー (paint で描画)
    auto grCell = row.removeFromLeft (cellW);
    grLabel.setBounds (grCell.removeFromTop (labelH));
    grMeterBounds = grCell.withSizeKeepingCentre (50, sliderH);

    area.removeFromTop (10);
    modeLabel.setBounds (area.removeFromTop (labelH));
    auto modeRow = area.removeFromTop (30);
    modeBox.setBounds (modeRow.withSizeKeepingCentre (200, 28));
}

void KyoheiClipperEditor::timerCallback()
{
    const float raw = proc.grPeakDb.load (std::memory_order_relaxed);

    // attack 即時、release は緩く下降
    grSmoothed = juce::jmax (raw, grSmoothed * 0.82f);

    // peak hold (1 秒)
    if (raw >= grPeakHold)
    {
        grPeakHold = raw;
        peakHoldFramesLeft = 30;
    }
    else if (peakHoldFramesLeft > 0)
    {
        --peakHoldFramesLeft;
    }
    else
    {
        grPeakHold = juce::jmax (grPeakHold - 0.25f, grSmoothed);
    }

    repaint (grMeterBounds.expanded (0, 24)); // 数値表示分も含めて再描画
}

void KyoheiClipperEditor::drawGrMeter (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    constexpr float maxDb = 12.0f;

    // 背景
    g.setColour (juce::Colour::fromRGB (16, 16, 20));
    g.fillRect (bounds);
    g.setColour (juce::Colour::fromRGB (60, 60, 70));
    g.drawRect (bounds, 1);

    // 目盛 (3, 6, 9 dB)
    g.setColour (juce::Colour::fromRGB (45, 45, 55));
    for (float tick : { 3.0f, 6.0f, 9.0f })
    {
        const float yFrac = tick / maxDb;
        const int y = bounds.getY() + (int) std::round (yFrac * bounds.getHeight());
        g.drawHorizontalLine (y, (float) bounds.getX(), (float) bounds.getRight());
    }

    // 現在値バー (上から下へ伸びる、緑→黄→赤)
    const float curClamped = juce::jlimit (0.0f, maxDb, grSmoothed);
    if (curClamped > 0.001f)
    {
        const int barH = (int) std::round ((curClamped / maxDb) * bounds.getHeight());
        auto barRect = bounds.withHeight (barH);

        juce::ColourGradient grad (
            juce::Colour::fromRGB (80, 220, 100),  // top (small GR) = 緑
            (float) bounds.getX(), (float) bounds.getY(),
            juce::Colour::fromRGB (240, 80, 60),   // bottom (heavy GR) = 赤
            (float) bounds.getX(), (float) bounds.getBottom(),
            false);
        grad.addColour (0.45, juce::Colour::fromRGB (240, 200, 80)); // mid = 黄
        g.setGradientFill (grad);
        g.fillRect (barRect);
    }

    // ピークホールド (横線)
    const float peakClamped = juce::jlimit (0.0f, maxDb, grPeakHold);
    if (peakClamped > 0.001f)
    {
        const int y = bounds.getY()
                    + (int) std::round ((peakClamped / maxDb) * bounds.getHeight());
        g.setColour (juce::Colours::white);
        g.drawHorizontalLine (y, (float) bounds.getX(), (float) bounds.getRight());
    }

    // 数値表示 (バー下: 現在 / ピーク dB)
    auto textArea = juce::Rectangle<int> (bounds.getX() - 10, bounds.getBottom() + 2,
                                          bounds.getWidth() + 20, 20);
    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (11.0f)));
    g.drawText (juce::String (grSmoothed, 1) + " / " + juce::String (grPeakHold, 1) + " dB",
                textArea, juce::Justification::centred);
}
