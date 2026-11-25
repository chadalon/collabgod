# The CollabGod desktop app.
#
# For now this is the skeleton the build system needs: a window, the engine linked
# in, and a smoke test. The web view host and bridge come with #26, the engine
# projection with #29.

juce_add_gui_app(collabgod
    PRODUCT_NAME "CollabGod"
    COMPANY_NAME "CollabGod"
    BUNDLE_ID "dev.collabgod.app"
    VERSION ${PROJECT_VERSION}
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER TRUE
    # Windows: use WebView2 (Edge), never JUCE's Internet Explorer default (ADR-0002).
    NEEDS_WEBVIEW2 TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "CollabGod opens your audio device, which may include inputs.")

juce_generate_juce_header(collabgod)

target_sources(collabgod PRIVATE
    src/Main.cpp
    src/SmokeTest.cpp
    src/SmokeTest.h)

target_compile_definitions(collabgod PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=1
    # Link the WebView2 loader statically, so there is no DLL to ship (ADR-0002, #8).
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:collabgod,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:collabgod,JUCE_VERSION>"
    COLLABGOD_TRACKTION_COMMIT="${COLLABGOD_TRACKTION_COMMIT}"
    COLLABGOD_JUCE_COMMIT="${COLLABGOD_JUCE_COMMIT}"
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1
    TRACKTION_LOG_DEVICES=0)

target_link_libraries(collabgod
    PRIVATE
        tracktion::tracktion_core
        tracktion::tracktion_engine
        tracktion::tracktion_graph
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(COLLABGOD_LTO)
    target_link_libraries(collabgod PUBLIC juce::juce_recommended_lto_flags)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(collabgod PRIVATE atomic)
endif()

if(APPLE)
    # Tracktion Engine needs this with Apple clang, as its own examples do.
    target_compile_options(collabgod PRIVATE -fno-aligned-allocation)
endif()

# ---------------------------------------------------------------------------
# Tests. They need no window and no audio device, so they run on CI runners.
# On Linux, JUCE's GUI app still wants a display: run ctest under xvfb-run.
# ---------------------------------------------------------------------------

add_test(NAME app_version COMMAND collabgod --version)
set_tests_properties(app_version PROPERTIES
    TIMEOUT 60
    PASS_REGULAR_EXPRESSION "CollabGod ${PROJECT_VERSION}")

add_test(NAME app_smoke_test COMMAND collabgod --smoke-test)
set_tests_properties(app_smoke_test PROPERTIES TIMEOUT 120)

if(APPLE)
    # The deployment target set once in the root CMakeLists.txt reaches the binary.
    add_test(NAME app_macos_minimum
        COMMAND ${CMAKE_COMMAND}
            -DBINARY=$<TARGET_FILE:collabgod>
            -DEXPECTED=${CMAKE_OSX_DEPLOYMENT_TARGET}
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckMacosMinimum.cmake)
endif()
