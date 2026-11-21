# Spike for issue #12: mirror VST3 parameter changes between two app instances.
# Throwaway code. See README.md for the findings and how to build and run it.

cmake_minimum_required(VERSION 3.22)

project(CollabGodPluginParamsSpike VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Oldest macOS the spike runs on")
endif()

if(MSVC)
    # Static runtime, as Tracktion Engine's own examples use.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

enable_testing()

# ---------------------------------------------------------------------------
# The mirroring rules for parameters (#12) and state (#13): plain C++20, no
# JUCE, no engine. Tested on their own.
# ---------------------------------------------------------------------------

add_library(param_sync_core STATIC
    src/core/ParamSync.cpp src/core/ParamSync.h
    src/core/StateSync.cpp src/core/StateSync.h)
target_include_directories(param_sync_core PUBLIC src/core)

if(MSVC)
    target_compile_options(param_sync_core PRIVATE /W4)
else()
    target_compile_options(param_sync_core PRIVATE -Wall -Wextra -Wshadow -Wconversion)
endif()

add_executable(param_sync_tests tests/ParamSyncTests.cpp)
target_link_libraries(param_sync_tests PRIVATE param_sync_core)
add_test(NAME sync_rules COMMAND param_sync_tests)

# Issue #13: the state sync rules, on the same clock as the parameter rules.
add_executable(state_sync_tests tests/StateSyncTests.cpp)
target_link_libraries(state_sync_tests PRIVATE param_sync_core)
add_test(NAME state_rules COMMAND state_sync_tests)

# ---------------------------------------------------------------------------
# Everything else needs the same pinned JUCE 9 and Tracktion Engine as
# spikes/engine-toolchain (ADR-0009). -DPLUGIN_PARAMS_CORE_ONLY=ON skips them,
# to work on the rules without the long engine build.
# ---------------------------------------------------------------------------

option(PLUGIN_PARAMS_CORE_ONLY "Build only the sync rules and their tests" OFF)

if(PLUGIN_PARAMS_CORE_ONLY)
    return()
endif()

include(../engine-toolchain/cmake/EngineDependencies.cmake)

set(PLUGIN_PARAMS_DEFINITIONS
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_PLUGINHOST_VST3=1
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1
    TRACKTION_LOG_DEVICES=0)

# ---------------------------------------------------------------------------
# The test plugin, twice. Version 2 adds a parameter at the front of the list,
# so every other parameter moves to a new index but keeps its ID.
# ---------------------------------------------------------------------------

foreach(version 1 2)
    juce_add_plugin(test_synth_v${version}
        VERSION ${version}.0.0
        PRODUCT_NAME "CollabGod Test Synth"
        COMPANY_NAME "CollabGod"
        BUNDLE_ID "dev.collabgod.testsynth"
        PLUGIN_MANUFACTURER_CODE Cgod
        PLUGIN_CODE Cgts
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        FORMATS VST3
        VST3_CATEGORIES Instrument Synth
        COPY_PLUGIN_AFTER_BUILD FALSE)

    juce_generate_juce_header(test_synth_v${version})
    target_sources(test_synth_v${version} PRIVATE test-plugin/TestSynth.cpp)

    target_compile_definitions(test_synth_v${version} PUBLIC
        TEST_SYNTH_VERSION=${version}
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        # JUCE plugins otherwise add 2,048 hidden "MIDI CC" parameters to every VST3.
        JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_link_libraries(test_synth_v${version}
        PRIVATE
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endforeach()

# ---------------------------------------------------------------------------
# The app. With no arguments it opens a window for trying real plugins between
# two machines (README, "Try it with real plugins"). With --self-test it runs
# three members on this machine over TCP with the test plugin, opens no window
# and no audio device, and exits 0 if every check passes; ctest runs that. One
# executable for both, so Tracktion Engine is compiled once.
# ---------------------------------------------------------------------------

juce_add_gui_app(plugin_params_spike
    PRODUCT_NAME "CollabGod Plugin Params Spike"
    COMPANY_NAME "CollabGod"
    BUNDLE_ID "dev.collabgod.pluginparamsspike"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "The spike opens the default audio device, which may include inputs. It records nothing.")

juce_generate_juce_header(plugin_params_spike)

target_sources(plugin_params_spike PRIVATE
    src/app/Main.cpp
    src/host/Link.cpp
    src/host/Link.h
    src/host/PluginSession.cpp
    src/host/PluginSession.h
    src/host/StateHost.cpp
    src/host/StateHost.h
    src/host/Wire.cpp
    src/host/Wire.h
    src/probe/StateProbe.cpp
    src/selftest/Harness.h
    src/selftest/SelfTest.cpp
    src/selftest/SelfTest.h
    src/selftest/StateSelfTest.cpp)

# Mouse buttons as the system sees them (finding 16): AppKit on macOS, JUCE elsewhere.
if(APPLE)
    target_sources(plugin_params_spike PRIVATE src/app/MouseButtons.h src/app/MouseButtons_mac.mm)
else()
    target_sources(plugin_params_spike PRIVATE src/app/MouseButtons.h src/app/MouseButtons.cpp)
endif()

target_compile_definitions(plugin_params_spike PRIVATE
    ${PLUGIN_PARAMS_DEFINITIONS}
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:plugin_params_spike,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:plugin_params_spike,JUCE_VERSION>")

target_link_libraries(plugin_params_spike
    PRIVATE
        param_sync_core
        tracktion::tracktion_core
        tracktion::tracktion_engine
        tracktion::tracktion_graph
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(plugin_params_spike PRIVATE atomic)
endif()

if(APPLE)
    # Tracktion Engine's examples need this with Apple clang.
    target_compile_options(plugin_params_spike PRIVATE -fno-aligned-allocation)
endif()

# The test plugins are built first; the self-test loads them from their build folders.
add_dependencies(plugin_params_spike test_synth_v1_VST3 test_synth_v2_VST3)

add_test(NAME mirror_between_members
    COMMAND plugin_params_spike --self-test
        --v1 "$<GENEX_EVAL:$<TARGET_PROPERTY:test_synth_v1_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>"
        --v2 "$<GENEX_EVAL:$<TARGET_PROPERTY:test_synth_v2_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>"
        --log-dir ${CMAKE_BINARY_DIR}/selftest)
set_tests_properties(mirror_between_members PROPERTIES TIMEOUT 180)

# Issue #13: the plugin's saved state between three members, with the same test VST3.
add_test(NAME state_between_members
    COMMAND plugin_params_spike --self-test-state
        --v1 "$<GENEX_EVAL:$<TARGET_PROPERTY:test_synth_v1_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>"
        --v2 "$<GENEX_EVAL:$<TARGET_PROPERTY:test_synth_v2_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>"
        --log-dir ${CMAKE_BINARY_DIR}/selftest-state)
set_tests_properties(state_between_members PROPERTIES TIMEOUT 300)

# The probe that measures real plugins, run on both versions of the test VST3,
# each in its own process as it runs on a folder of real plugins.
add_test(NAME state_probe
    COMMAND plugin_params_spike
        --probe-state "$<TARGET_FILE_DIR:test_synth_v1>/.."
        --out ${CMAKE_BINARY_DIR}/selftest-state/probe.md --try-bad-state)
set_tests_properties(state_probe PROPERTIES TIMEOUT 300
    PASS_REGULAR_EXPRESSION "CollabGod Test Synth \\(CollabGod\\) \\| 1\\.0\\.0.*Report written")
