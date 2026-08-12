// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
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
    // OS 群遅延 + look-ahead ぶんの音声を内部に保持するため、入力停止後もその分の
    // 出力が続く。0 を返すとオフラインバウンス時に末尾を切り落とすホストがあるので、
    // 実レイテンシを tail として報告する (K Slew Limiter / K Peak Controller と同基準)。
    double getTailLengthSeconds() const override
    {
        return preparedSampleRate > 0.0 ? (double) getLatencySamples() / preparedSampleRate : 0.0;
    }

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

    // 出力ピーク [dBFS] — output gain 適用後の True-Peak 推定。ベースレート sample peak と
    // 「OS ドメイン処理後ピーク × 出力ゲイン × wet 比率」の大きい方 (ダウンサンプルで生じる
    // inter-sample peak の取りこぼし防止)。クリップ LED 判定 (0 dBFS 超え) に使う
    std::atomic<float> outputPeakDb { -100.0f };

    // ---- 情報行 (Peak / Max GR)。prepareToPlay と情報行クリックでクリア ----
    // Peak = input gain 適用後のセッション最大サンプルピーク [dBFS]、
    // Max GR = セッション最大 GR [dB] (どちらも実測値。予測値ではない)。
    std::atomic<float> sessionPeakDb { -100.0f };
    std::atomic<float> sessionGrDb   { 0.0f };

    /** 情報行のホールドをクリアする (UI の情報行クリック)。
        message thread から安全: atomic ストアのみ。 */
    void resetSessionPeaks() noexcept
    {
        sessionPeakDb.store (-100.0f, std::memory_order_relaxed);
        sessionGrDb.store   (0.0f,    std::memory_order_relaxed);
    }

    // ---- ビジュアライザ (スクロール表示、L/R 独立。K Peak Controller と同設計) ----
    // 10ms フレームごとにチャンネル別の { 入力ピーク dBFS, GR dB } をリングに書く。
    // どちらも「input gain 適用後の入力タイムライン」上の値で揃えてある。総レイテンシ
    // (OS 群遅延 + 0.2ms look-ahead) は 1 フレーム = 10ms よりはるかに短いので、この
    // 粒度では両者のズレは見えない。
    static constexpr int kVisFrames = 1024;   // 約 10 秒
    std::array<std::array<std::atomic<float>, kVisFrames>, 2> visInDb;
    std::array<std::array<std::atomic<float>, kVisFrames>, 2> visGrDb;
    std::atomic<int> visWritePos { 0 };       // release で公開 (UI は acquire で読む)
    std::atomic<int> visNumChannels { 2 };    // モノトラック時は 1 レーン表示

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

    // --- ビジュアライザ用スクラッチ (base レート、ch 別) ---
    // gainScratch: OS ドメインで実際に適用したゲインを base サンプル単位に min で畳んだもの。
    // visInScratch: input gain 適用直後の |入力| (OS 後の buffer は処理済み信号なので別に保持)。
    juce::AudioBuffer<float> gainScratch;
    juce::AudioBuffer<float> visInScratch;
    int visFrameLen = 480;                  // 10ms 相当 (prepareToPlay で算出)
    int visFrameCount = 0;
    std::array<float, 2> visFramePeak { { 0.0f, 0.0f } };
    std::array<float, 2> visFrameMinGain { { 1.0f, 1.0f } };

    // processBlock 1回分の実処理。numSamples <= preparedBlockSize が前提
    // (processBlock 側で分割保証済み)。
    void processChunk (juce::AudioBuffer<float>&);

    // prepareToPlay で告知された最大ブロック長。dryScratch/oversampler の確保量はこれ前提
    // なので、超過ブロックを渡す契約違反ホストでは processBlock がこのサイズに分割処理する。
    int preparedBlockSize = 0;
    double preparedSampleRate = 0.0;   // getTailLengthSeconds() でサンプル→秒に直すのに使う

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
