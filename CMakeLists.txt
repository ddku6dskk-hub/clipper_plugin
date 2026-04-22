cmake_minimum_required(VERSION 3.22)
project(KyoheiClipper VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "minimum macOS version")

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../JUCE JUCE)

# ============================================================
# AAX SDK 自動検出 (任意)
# ----------------------------------------------------------
# 優先順位:
#   1. cmake -DAAX_SDK_PATH=/path/to/AAX_SDK_2.x.x で明示指定
#   2. 環境変数 AAX_SDK_PATH
#   3. ~/SDKs/AAX_SDK_* の最新版を自動検出
# 見つからない場合: VST3/AU/Standalone のみ (現状互換)
# ============================================================
set(KYOHEI_FORMATS VST3 AU Standalone)

if(NOT DEFINED AAX_SDK_PATH AND DEFINED ENV{AAX_SDK_PATH})
    set(AAX_SDK_PATH "$ENV{AAX_SDK_PATH}")
endif()

if(NOT DEFINED AAX_SDK_PATH)
    file(GLOB _aax_candidates "$ENV{HOME}/SDKs/AAX_SDK_*"
                              "$ENV{HOME}/SDKs/AAX-SDK-*"
                              "$ENV{HOME}/Developer/AAX_SDK_*")
    list(SORT _aax_candidates)
    list(REVERSE _aax_candidates) # 最新版を先頭に
    foreach(_p IN LISTS _aax_candidates)
        if(IS_DIRECTORY "${_p}")
            set(AAX_SDK_PATH "${_p}")
            break()
        endif()
    endforeach()
endif()

if(AAX_SDK_PATH AND IS_DIRECTORY "${AAX_SDK_PATH}")
    message(STATUS "AAX SDK found: ${AAX_SDK_PATH}")
    juce_set_aax_sdk_path("${AAX_SDK_PATH}")
    list(APPEND KYOHEI_FORMATS AAX)
else()
    message(STATUS "AAX SDK not found (set -DAAX_SDK_PATH=... or env AAX_SDK_PATH to enable). Building VST3/AU/Standalone only.")
endif()

# 共通の source 群
set(KYOHEI_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

set(KYOHEI_COMMON_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=1
)

set(KYOHEI_COMMON_LIBS
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# ===== Kyohei Clipper (マスタリング向け, 16x OS + lookahead) =====
juce_add_plugin(KyoheiClipper
    COMPANY_NAME "K Audio"
    BUNDLE_ID com.kaudio.clipper
    PLUGIN_MANUFACTURER_CODE Khyk
    PLUGIN_CODE Kcl1
    FORMATS ${KYOHEI_FORMATS}
    PRODUCT_NAME "K Clipper"
    DESCRIPTION "Transparent mastering clipper with look-ahead ISP limiter."
    HARDENED_RUNTIME_ENABLED TRUE
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    AU_COPY_DIR  "/Library/Audio/Plug-Ins/Components"
    VST3_COPY_DIR "/Library/Audio/Plug-Ins/VST3"
    AAX_COPY_DIR "/Library/Application Support/Avid/Audio/Plug-Ins"
    AAX_CATEGORY AAX_ePlugInCategory_Dynamics
)

target_sources(KyoheiClipper PRIVATE ${KYOHEI_SOURCES})
target_include_directories(KyoheiClipper PRIVATE Source)
target_compile_definitions(KyoheiClipper PRIVATE ${KYOHEI_COMMON_DEFS} KYOHEI_SLAMMER=0)
target_link_libraries(KyoheiClipper PRIVATE ${KYOHEI_COMMON_LIBS})

# ===== Kyohei Slammer (ドラム/ベース向け, 4x OS, lookahead なし) =====
juce_add_plugin(KyoheiSlammer
    COMPANY_NAME "K Audio"
    BUNDLE_ID com.kaudio.slammer
    PLUGIN_MANUFACTURER_CODE Khyk
    PLUGIN_CODE Ksl1
    FORMATS ${KYOHEI_FORMATS}
    PRODUCT_NAME "K Slammer"
    DESCRIPTION "Transient-preserving clipper for drums and bass."
    HARDENED_RUNTIME_ENABLED TRUE
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    AU_COPY_DIR  "/Library/Audio/Plug-Ins/Components"
    VST3_COPY_DIR "/Library/Audio/Plug-Ins/VST3"
    AAX_COPY_DIR "/Library/Application Support/Avid/Audio/Plug-Ins"
    AAX_CATEGORY AAX_ePlugInCategory_Dynamics
)

target_sources(KyoheiSlammer PRIVATE ${KYOHEI_SOURCES})
target_include_directories(KyoheiSlammer PRIVATE Source)
target_compile_definitions(KyoheiSlammer PRIVATE ${KYOHEI_COMMON_DEFS} KYOHEI_SLAMMER=1)
target_link_libraries(KyoheiSlammer PRIVATE ${KYOHEI_COMMON_LIBS})
