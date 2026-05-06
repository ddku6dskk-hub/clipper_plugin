// SPDX-License-Identifier: GPL-3.0-or-later
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

    // GR メーター: ラベル + CLIP LED 行 + 縦バー (paint で描画)
    // GR ラベル下に CLIP LED 行 18px を確保 → bar はその分縮める
    constexpr int clipRowH = 18;
    auto grCell = row.removeFromLeft (cellW);
    grLabel.setBounds (grCell.removeFromTop (labelH));
    grCell.removeFromTop (clipRowH);
    grMeterBounds = grCell.withSizeKeepingCentre (50, sliderH - clipRowH);

    area.removeFromTop (10);
    modeLabel.setBounds (area.removeFromTop (labelH));
    auto modeRow = area.removeFromTop (30);
    modeBox.setBounds (modeRow.withSizeKeepingCentre (200, 28));
}

void KyoheiClipperEditor::timerCallback()
{
    // GR (Gain Reduction)
    const float rawGr = proc.grPeakDb.load (std::memory_order_relaxed);
    const float safeRawGr = (std::isfinite (rawGr) && rawGr >= 0.0f) ? rawGr : 0.0f;
    grSmoothed = juce::jmax (safeRawGr, grSmoothed * 0.82f);
    if (safeRawGr >= grPeakHold)
    {
        grPeakHold = safeRawGr;
        peakHoldFramesLeft = 30; // 1 秒 (30Hz)
    }
    else if (peakHoldFramesLeft > 0)
    {
        --peakHoldFramesLeft;
    }
    else
    {
        grPeakHold = juce::jmax (grPeakHold - 0.25f, grSmoothed);
    }

    // 入力サンプルピーク (dBFS): attack 即時、release 緩やか、ピークホールド 1秒
    const float rawIn = proc.inputPeakDb.load (std::memory_order_relaxed);
    const float safeRawIn = std::isfinite (rawIn) ? juce::jlimit (-100.0f, 0.0f, rawIn) : -100.0f;
    inputPeakSmoothed = juce::jlimit (-100.0f, 0.0f,
                                      juce::jmax (safeRawIn, inputPeakSmoothed - 1.5f));
    if (safeRawIn >= inputPeakHold)
    {
        inputPeakHold = safeRawIn;
        inputPeakHoldFramesLeft = 30;
    }
    else if (inputPeakHoldFramesLeft > 0)
    {
        --inputPeakHoldFramesLeft;
    }
    else
    {
        inputPeakHold = juce::jlimit (-100.0f, 0.0f,
                                      juce::jmax (inputPeakHold - 0.5f, inputPeakSmoothed));
    }

    // クリップ LED: 出力 peak が 0 dBFS を超えたら点灯、30 frame (= 1秒) ホールド
    float rawOut = proc.outputPeakDb.load (std::memory_order_relaxed);
    if (! std::isfinite (rawOut))
        rawOut = -100.0f;
    if (rawOut > 0.0f)
        clipLedFramesLeft = 30;
    else if (clipLedFramesLeft > 0)
        --clipLedFramesLeft;

    // repaint 範囲: 横は数値テキスト (130px) 用に拡張、上は CLIP LED 用に広め
    const int meterCx = grMeterBounds.getCentreX();
    repaint (meterCx - 70,
             grMeterBounds.getY() - 28,
             140,
             grMeterBounds.getHeight() + 52);
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

    // 数値表示 (バー下: 入力ピーク dBFS / GRピーク dB)
    // input が 0 dBFS に達した瞬間以降 (1秒ピークホールド期間中) は dBFS 値を赤く描画
    // text area: cell 幅 (130px) いっぱいに広げて折り返しを防ぐ
    auto textArea = juce::Rectangle<int> (bounds.getCentreX() - 65, bounds.getBottom() + 2,
                                          130, 20);
    const float textInputPeak = juce::jlimit (-100.0f, 0.0f, inputPeakHold);
    const float textGrPeak = juce::jlimit (0.0f, 99.9f, grPeakHold);
    const juce::String inText = juce::String (textInputPeak, 1) + " dBFS";
    const juce::String grText = juce::String (textGrPeak,    1) + " dB";
    const juce::String separator { "  /  " };

    const bool inputClip = (inputPeakHold >= -0.01f); // ほぼ 0dBFS or 上回った
    const juce::Colour inColour = inputClip
        ? juce::Colour::fromRGB (255, 90, 80)
        : juce::Colours::white;

    juce::AttributedString att;
    juce::Font textFont (juce::FontOptions (10.0f));
    att.append (inText,                textFont, inColour);
    att.append (separator + grText,    textFont, juce::Colours::white);
    att.setJustification (juce::Justification::centred);
    att.setWordWrap (juce::AttributedString::none);  // 折返し禁止
    att.draw (g, textArea.toFloat());

    // クリップ LED (HA 風): バー上部に小さな赤 LED + "CLIP" ラベル横並び
    constexpr int ledDiameter = 8;
    const int rowY = bounds.getY() - 14;             // バー上 14px の余白
    const int rowH = ledDiameter + 4;
    const bool clipActive = clipLedFramesLeft > 0;

    // LED 円 (左寄せ)
    const int ledX = bounds.getX() + 6;
    auto ledBounds = juce::Rectangle<float> ((float) ledX, (float) rowY,
                                              (float) ledDiameter, (float) ledDiameter);
    if (clipActive)
    {
        g.setColour (juce::Colour::fromRGB (255, 60, 50));
        g.fillEllipse (ledBounds);
        g.setColour (juce::Colour::fromRGB (255, 200, 180));
        g.drawEllipse (ledBounds, 1.0f);
    }
    else
    {
        g.setColour (juce::Colour::fromRGB (60, 20, 22));
        g.fillEllipse (ledBounds);
        g.setColour (juce::Colour::fromRGB (40, 40, 48));
        g.drawEllipse (ledBounds, 1.0f);
    }

    // "CLIP" ラベル (LED の右隣)
    g.setColour (clipActive ? juce::Colour::fromRGB (255, 90, 80)
                            : juce::Colour::fromRGB (90, 90, 100));
    g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
    auto labelArea = juce::Rectangle<int> (ledX + ledDiameter + 4, rowY - 1,
                                            bounds.getWidth(), rowH);
    g.drawText ("CLIP", labelArea, juce::Justification::centredLeft);
}
