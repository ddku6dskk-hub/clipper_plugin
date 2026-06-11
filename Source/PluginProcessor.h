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
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    // ホスト bypass を内部 soft-bypass パラメータに紐づける。これにより host bypass 時も
    // processBlock が呼ばれ続け、wet↔dry をクロスフェードしてクリックレスにできる。
    juce::AudioProcessorParameter* getBypassParameter() const override;

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

    // GR (Gain Reduction) 量 [dB] — limiter/shaper が OS ドメインで実際に適用したゲイン係数から
    // 直接算出 (入出力ピーク比ではない)。閾値以下の素通し区間は構造的に 0 dB。
    // UI 側で Timer から読み取って描画する
    std::atomic<float> grPeakDb { 0.0f };

    // 入力サンプルピーク [dBFS] — input gain 適用後 (= ユーザーが設定した Input ノブ反映済み)
    std::atomic<float> inputPeakDb { -100.0f };

    // 出力サンプルピーク [dBFS] — output gain 適用後 (= 後段に送る最終信号レベル)
    // クリップ LED 判定 (0 dBFS 超え) に使う
    std::atomic<float> outputPeakDb { -100.0f };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    // 16x oversampling + OS 内 lookahead limiter + soft clipper のハイブリッド
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    std::array<kyohei::dsp::LookaheadLimiter<float>, 2> limiters; // stereo L/R
    std::array<kyohei::dsp::ClipperChain<float>, 2> chains;       // stereo L/R

    // --- click-free soft bypass ---
    // dry(input gain 適用前の素入力)を wet と同じ reportedLatency だけ遅延させて時間整合し、
    // bypassMix で per-sample クロスフェードする。bypass 状態に関わらず常時 DSP を回すことで
    // 復帰時の OS/IIR/limiter 状態不連続も防ぐ。
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dryDelayLine;
    juce::SmoothedValue<float> bypassMix;   // 0 = active, 1 = bypassed (15ms ramp)
    juce::AudioBuffer<float> dryScratch;    // 遅延済み dry の一時保持 (wet 処理後に混ぜる)

    // processBlock 1回分の実処理。numSamples <= preparedBlockSize が前提
    // (processBlock 側で分割保証済み)。
    void processChunk (juce::AudioBuffer<float>&);

    // prepareToPlay で告知された最大ブロック長。dryScratch/oversampler の確保量はこれ前提
    // なので、超過ブロックを渡す契約違反ホストでは processBlock がこのサイズに分割処理する。
    int preparedBlockSize = 0;

    // input/output gain の前ブロック適用値。applyGainRamp で今ブロック値へ線形補間し、
    // オートメーション/ノブ操作時のブロック境界段差 (ジッパーノイズ) を防ぐ。
    float lastInGain  = 1.0f;
    float lastOutGain = 1.0f;

    std::atomic<float>* pThreshold = nullptr;
    std::atomic<float>* pKnee = nullptr;
    std::atomic<float>* pMode = nullptr;
    std::atomic<float>* pInputGain = nullptr;
    std::atomic<float>* pOutputGain = nullptr;
    std::atomic<float>* pBypass = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KyoheiClipperProcessor)
};
