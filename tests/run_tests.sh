#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# 全テストをビルドして実行する。plugin/ 直下から `sh tests/run_tests.sh` で起動。
#
# 1-3 は DSP ヘッダ直結 (JUCE 不要のものは JUCE のインクルードパスだけ使う)。
# 4 は本番 Processor を通す統合テストなので、SharedCode 静的ライブラリが要る
#   → 先に一度 `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release`
#   ライブラリが無い場合は **FAIL 扱い**。承知のうえで 1-3 だけ回すなら
#   ALLOW_SKIP_INTEGRATION=1 を付ける (その場合は結果表示も「検品完了ではない」になる)。
set -u
skipped=0

JUCE_MODULES="${JUCE_MODULES:-$HOME/Developer/_sdk/JUCE/modules}"
OUT="${TMPDIR:-/tmp}/kclipper-tests"
LIB="build/KyoheiClipper_artefacts/Release/libK Clipper_SharedCode.a"
mkdir -p "$OUT"
fail=0

run() {   # run <名前> <実行ファイル>
    if "$2"; then
        echo "  -> PASS: $1"
    else
        echo "  -> FAIL: $1"
        fail=$((fail + 1))
    fi
    echo
}

echo "== 1. limiter lookahead =="
clang++ -std=c++17 -O2 -fsanitize=address,undefined \
    tests/limiter_lookahead_test.cpp -o "$OUT/lookahead" || exit 1
run "limiter lookahead" "$OUT/lookahead"

echo "== 2. limiter threshold smoothing =="
clang++ -std=c++17 -O2 -fsanitize=address,undefined \
    tests/limiter_threshold_smooth_test.cpp -o "$OUT/smooth" || exit 1
run "limiter threshold smoothing" "$OUT/smooth"

echo "== 3. linked shelf precision =="
clang++ -std=c++20 -O2 -DNDEBUG=1 -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 \
    -DJUCE_MODULE_AVAILABLE_juce_dsp=1 -DJUCE_STANDALONE_APPLICATION=1 \
    -DJUCE_CHECK_MEMORY_LEAKS=0 -I "$JUCE_MODULES" \
    tests/linked_shelf_precision_test.cpp -o "$OUT/shelf" || exit 1
run "linked shelf precision" "$OUT/shelf"

echo "== 4. processor integration (K Clipper / K Slammer) =="
# 【重要】統合テストを飛ばしたまま「ALL TESTS PASSED」を返してはいけない。
# 必須テストを省いた実行が成功扱いになると、検品したつもりで素通りする。
# 部分実行したいときだけ ALLOW_SKIP_INTEGRATION=1 を明示する。
if [ ! -f "$LIB" ]; then
    if [ "${ALLOW_SKIP_INTEGRATION:-0}" = "1" ]; then
        echo "  -> SKIP (ALLOW_SKIP_INTEGRATION=1): $LIB がありません"
        echo
        skipped=1
    else
        echo "  -> FAIL: $LIB がありません。先に Release ビルドしてください:"
        echo "     cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release"
        echo "     (承知のうえで 1-3 だけ回すなら ALLOW_SKIP_INTEGRATION=1 を付ける)"
        echo
        fail=$((fail + 1))
    fi
else
    MODS="audio_processors audio_processors_headless gui_extra gui_basics graphics events core \
          data_structures audio_basics audio_utils audio_formats audio_devices dsp"
    DEFS=""
    for m in $MODS; do DEFS="$DEFS -DJUCE_MODULE_AVAILABLE_juce_$m=1"; done
    for slammer in 0 1; do
        name="clipper"; [ "$slammer" = "1" ] && name="slammer"
        clang++ -std=c++20 -O2 -DNDEBUG=1 -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 \
            -DJUCE_STANDALONE_APPLICATION=0 -DJUCE_WEB_BROWSER=0 -DJUCE_USE_CURL=0 \
            -DKYOHEI_SLAMMER=$slammer $DEFS \
            -I "$JUCE_MODULES" -I Source \
            tests/processor_regression_test.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp \
            "$LIB" \
            -framework CoreAudioKit -framework DiscRecording -framework WebKit \
            -framework Metal -framework MetalKit -framework QuartzCore \
            -framework CoreAudio -framework CoreMIDI -framework AudioToolbox \
            -framework Accelerate -framework Cocoa -framework Foundation \
            -framework IOKit -framework Security \
            -o "$OUT/processor_$name" || exit 1
        run "processor integration ($name)" "$OUT/processor_$name"
    done
fi

if [ "$fail" -ne 0 ]; then
    echo "$fail TEST(S) FAILED"
elif [ "$skipped" -ne 0 ]; then
    echo "PASSED WITH SKIPS — 統合テスト未実行。**これは検品完了ではない**"
else
    echo "ALL TESTS PASSED"
fi
exit "$fail"
