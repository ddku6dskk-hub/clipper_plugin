// SPDX-License-Identifier: GPL-3.0-or-later
// LinkedShelf の pre->post 打ち消し精度テスト (本番ヘッダ直結)。
//
// 【背景】Open / LF モードは「閾値以下 (= shaper が介入しない区間) では pre と post が
// 打ち消して完全にフラット」を前提にしている。IIR の係数・状態を float で持つと、
// 16x OS (768kHz) に対して corner の低い LF モード (2604Hz → 正規化 0.0034) で
// 直接形バイキャッドの量子化誤差が効き、この打ち消しが閉じなくなる。
// 実測: float 実装は shelf -12dB / 入力 -6dBFS / 768kHz で **-81 dBFS** の残差を残していた。
// double 化により入出力境界の float 1 ULP (≒ -150 dBFS) まで落ちる。
//
// 【判定】残差 ≤ -130 dBFS を要求する (float 1 ULP = 約 -150 dBFS に対し十分な余裕。
// 逆に float 実装のままだと -81〜-99 dBFS なので確実に落ちる)。
//
// juce::dsp::IIR を使うので JUCE のインクルードパスが要る。IIR の非テンプレート実装だけを
// 直接取り込むことで、JUCE モジュール一式のビルド/リンクなしに単体実行できる:
//   c++ -std=c++20 -O2 -DNDEBUG=1 -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 \
//       -DJUCE_MODULE_AVAILABLE_juce_dsp=1 -DJUCE_STANDALONE_APPLICATION=1 \
//       -DJUCE_CHECK_MEMORY_LEAKS=0 \
//       -I "$HOME/Developer/_sdk/JUCE/modules" linked_shelf_precision_test.cpp \
//       -o /tmp/shelf_prec && /tmp/shelf_prec
#include "../Source/dsp/LinkedShelf.h"
#include <juce_dsp/processors/juce_IIRFilter.cpp>   // Coefficients の非 inline 実装

// juce_core.h が debug ビルドで張る「全 TU が同じ設定でビルドされたか」の番兵。
// 本テストは juce_core.cpp をリンクしないので、ここで定義を与えて解決する。
namespace juce {
#if JUCE_DEBUG
this_will_fail_to_link_if_some_of_your_compile_units_are_built_in_debug_mode
    ::this_will_fail_to_link_if_some_of_your_compile_units_are_built_in_debug_mode() noexcept {}
#else
this_will_fail_to_link_if_some_of_your_compile_units_are_built_in_release_mode
    ::this_will_fail_to_link_if_some_of_your_compile_units_are_built_in_release_mode() noexcept {}
#endif
}
#include <cstdio>
#include <cmath>
#include <random>
#include <algorithm>

using kyohei::dsp::LinkedShelf;

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { ++failures; printf("  FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// 「pre が実際に信号を変えているか」の記録。往復が閉じているだけで shelf が
// 素通しになっていたら (= 空振りテスト) 検出できるようにする。
static double lastPreDeviation = 0.0;

/** pre->post を通した往復誤差の最大値 [dBFS 相当] を返す。 */
static double roundTripDb (LinkedShelf<float>::Kind kind, float gainDb, float cornerHz,
                           float slope, int order, double fs, bool useFastGain)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = fs;
    spec.maximumBlockSize = 512;
    spec.numChannels = 1;

    LinkedShelf<float> shelf;
    shelf.prepare (spec, order);
    // setShelf でジオメトリを確定 → 動的ゲイン経路 (setGainDbFast) も同じ精度か確認する
    shelf.setShelf (kind, useFastGain ? 0.0f : gainDb, cornerHz, slope);
    if (useFastGain)
        shelf.setGainDbFast (gainDb);
    shelf.reset();

    std::mt19937 rng (7);
    std::uniform_real_distribution<float> dist (-0.5f, 0.5f);
    const int n = (int) fs;                   // 1 秒
    double maxErr = 0.0, maxPreDev = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float x = dist (rng);
        const float pre = shelf.processPre (x);
        const float y   = shelf.processPost (pre);
        if (i > 4000)                         // 起動過渡は除外
        {
            maxErr = std::max (maxErr, (double) std::abs (y - x));
            maxPreDev = std::max (maxPreDev, (double) std::abs (pre - x));
        }
    }
    lastPreDeviation = maxPreDev;
    return 20.0 * std::log10 (std::max (maxErr, 1e-30));
}

int main()
{
    constexpr double kLimitDb = -130.0;
    printf ("LinkedShelf pre->post 往復残差 (閾値以下の素通し区間の透明度)\n");
    printf ("判定基準: <= %.0f dBFS\n\n", kLimitDb);

    struct Case { const char* name; LinkedShelf<float>::Kind kind;
                  float gainDb, hz, slope; int order; double fs; bool fast; };
    const Case cases[] = {
        // Open モード (ClipperChain::prepare の実値)
        { "Open  +5.96dB @5979Hz  48k",   LinkedShelf<float>::Kind::HighShelf,  5.96f, 5979.f, 0.72f, 1,  48000.0, false },
        { "Open  +5.96dB @5979Hz 768k",   LinkedShelf<float>::Kind::HighShelf,  5.96f, 5979.f, 0.72f, 1, 768000.0, false },
        { "Open  +5.96dB @5979Hz 192k",   LinkedShelf<float>::Kind::HighShelf,  5.96f, 5979.f, 0.72f, 1, 192000.0, false },
        // LF モード (動的ゲイン: g = clamp(0.66 - 0.657*od, -12, 0))。setGainDbFast 経路も見る
        { "LF     0.00dB @2604Hz 768k",   LinkedShelf<float>::Kind::LowShelf,   0.00f, 2604.f, 0.37f, 2, 768000.0, true  },
        { "LF    -6.00dB @2604Hz 768k",   LinkedShelf<float>::Kind::LowShelf,  -6.00f, 2604.f, 0.37f, 2, 768000.0, true  },
        { "LF   -12.00dB @2604Hz 768k",   LinkedShelf<float>::Kind::LowShelf, -12.00f, 2604.f, 0.37f, 2, 768000.0, true  },
        { "LF   -12.00dB @2604Hz 192k",   LinkedShelf<float>::Kind::LowShelf, -12.00f, 2604.f, 0.37f, 2, 192000.0, true  },
        { "LF   -12.00dB @2604Hz  44k",   LinkedShelf<float>::Kind::LowShelf, -12.00f, 2604.f, 0.37f, 2,  44100.0, true  },
        // setShelf 直接指定の経路も一応
        { "LF   -12.00dB (setShelf) 768k", LinkedShelf<float>::Kind::LowShelf, -12.00f, 2604.f, 0.37f, 2, 768000.0, false },
    };

    for (const auto& c : cases)
    {
        const double db = roundTripDb (c.kind, c.gainDb, c.hz, c.slope, c.order, c.fs, c.fast);
        printf ("  %-32s %8.1f dBFS  (pre 偏差 %.4f)  %s\n",
                c.name, db, lastPreDeviation, db <= kLimitDb ? "OK" : "<-- NG");
        CHECK (db <= kLimitDb, "%s の往復残差が大きい (%.1f dBFS > %.0f)", c.name, db, kLimitDb);
        // 空振り防止: gain != 0 のケースは pre が必ず信号を変えていること
        // (shelf が素通しになっていたら往復誤差 0 で偽 PASS してしまう)
        if (std::abs (c.gainDb) > 0.01f)
            CHECK (lastPreDeviation > 1e-3, "%s: pre が信号を変えていない (空振りテスト)", c.name);
    }

    printf ("\n%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
