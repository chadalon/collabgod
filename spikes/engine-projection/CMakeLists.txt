# Spike for issue #11: the TypeScript session model drives Tracktion Engine.
# Throwaway code. See README.md for the findings and how to build and run it.

cmake_minimum_required(VERSION 3.22)

project(CollabGodEngineProjectionSpike VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Oldest macOS the spike runs on")
endif()

if(MSVC)
    # Static runtime, as Tracktion Engine's own examples use.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# The same pinned JUCE 9 and Tracktion Engine as spikes/engine-toolchain, with its
# Apple clang 15 patch (ADR-0009, engine-toolchain README finding 11).
include(../engine-toolchain/cmake/EngineDependencies.cmake)

juce_add_console_app(engine_projection
    PRODUCT_NAME "CollabGod Engine Projection Spike"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

juce_generate_juce_header(engine_projection)

target_sources(engine_projection PRIVATE
    src/Bench.cpp
    src/ClickDetector.cpp
    src/ClickDetector.h
    src/Commands.cpp
    src/Commands.h
    src/Host.cpp
    src/Host.h
    src/Main.cpp
    src/OutputAnalysis.cpp
    src/OutputAnalysis.h
    src/SelfTest.cpp
    src/Serve.cpp
    src/Wav.cpp
    src/Wav.h
    # The Goertzel tone measurement from the toolchain spike, used to check what played.
    ../engine-toolchain/src/ToneCheck.cpp
    ../engine-toolchain/src/ToneCheck.h)

target_include_directories(engine_projection PRIVATE ../engine-toolchain/src)

target_compile_definitions(engine_projection PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_PLUGINHOST_VST3=0
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1
    TRACKTION_LOG_DEVICES=0
    ENGINE_PROJECTION_CONTRACT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/contract")

target_link_libraries(engine_projection
    PRIVATE
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
    target_link_libraries(engine_projection PRIVATE atomic)
endif()

if(APPLE)
    # Tracktion Engine's examples need this with Apple clang.
    target_compile_options(engine_projection PRIVATE -fno-aligned-allocation)
endif()

# ---------------------------------------------------------------------------
# Tests. None needs an audio device: they use a simulated sound card on a
# real-time thread of our own.
# ---------------------------------------------------------------------------

enable_testing()

# Contract examples, edits changing what plays, a storm of edits during playback
# with the click detector watching, and a rebuild from scratch while playing.
add_test(NAME projection_selftest
    COMMAND engine_projection selftest --out ${CMAKE_BINARY_DIR}/selftest)
set_tests_properties(projection_selftest PROPERTIES TIMEOUT 180)
