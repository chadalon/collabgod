# The CollabGod desktop app: a window holding the interface in a web view, the
# bridge between them (#26) and the engine linked in. The engine projection comes
# with #29.
#
# It needs two things the TypeScript build makes, which `npm run build` at the
# repository root makes first:
#   packages/bridge/dist/cpp       the bridge contract, generated as C++ (BridgeContract.h)
#   packages/interface/dist        the interface's page, copied beside the app

set(COLLABGOD_BRIDGE_CPP_DIR "${PROJECT_SOURCE_DIR}/packages/bridge/dist/cpp")
set(COLLABGOD_UI_DIST_DIR "${PROJECT_SOURCE_DIR}/packages/interface/dist")

foreach(_needed "${COLLABGOD_BRIDGE_CPP_DIR}/BridgeContract.h" "${COLLABGOD_UI_DIST_DIR}/index.html")
    if(NOT EXISTS "${_needed}")
        message(FATAL_ERROR "${_needed} is missing. The TypeScript build makes it: run `npm run build` "
                            "(or `npm run build:ts`) at the repository root first.")
    endif()
endforeach()

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
    src/BridgeCodec.h
    src/BridgeHost.cpp
    src/BridgeHost.h
    src/BridgeTests.cpp
    src/BridgeTests.h
    src/Log.cpp
    src/Log.h
    src/Main.cpp
    src/SmokeTest.cpp
    src/SmokeTest.h
    src/UiFiles.cpp
    src/UiFiles.h
    src/WebUi.cpp
    src/WebUi.h
    src/WebViewBackground.h)

if(APPLE)
    target_sources(collabgod PRIVATE src/WebViewBackground_mac.mm)
else()
    target_sources(collabgod PRIVATE src/WebViewBackground.cpp)
endif()

# BridgeContract.h includes BridgeCodec.h from src/.
target_include_directories(collabgod PRIVATE src "${COLLABGOD_BRIDGE_CPP_DIR}")

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
    # --dev serves the page from here; Debug builds do by default and reload it when it changes.
    COLLABGOD_SOURCE_UI_DIR="${COLLABGOD_UI_DIST_DIR}"
    COLLABGOD_DEVELOPMENT=$<IF:$<CONFIG:Debug>,1,0>
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

if(WIN32)
    # SetWindowSubclass, for the window's background while it resizes (WebViewBackground.cpp).
    target_link_libraries(collabgod PRIVATE comctl32)
endif()

if(APPLE)
    # Tracktion Engine needs this with Apple clang, as its own examples do.
    target_compile_options(collabgod PRIVATE -fno-aligned-allocation)
endif()

# ---------------------------------------------------------------------------
# The interface, copied beside the app on every build, so the app runs without the
# source tree: Contents/Resources/ui in the macOS bundle, a `ui` folder next to the
# executable elsewhere. A target of its own, because a change to the page alone does
# not relink the app.
# ---------------------------------------------------------------------------

if(APPLE)
    set(_ui_destination "$<TARGET_BUNDLE_CONTENT_DIR:collabgod>/Resources/ui")
else()
    set(_ui_destination "$<TARGET_FILE_DIR:collabgod>/ui")
endif()

add_custom_target(collabgod_ui ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_ui_destination}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${COLLABGOD_UI_DIST_DIR}" "${_ui_destination}"
    COMMENT "Copying the interface beside the app"
    VERBATIM)
add_dependencies(collabgod_ui collabgod)

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

# The bridge contract on the app's side: every message's example through the
# generated codec and dispatch, and the real BridgeHost. No window.
add_test(NAME app_bridge_contract COMMAND collabgod --bridge-test)
set_tests_properties(app_bridge_contract PROPERTIES TIMEOUT 60)

# The whole bridge through the real web view: opens the window, the page calls every
# function, receives every event and sends every command, then the window is closed
# as its close button closes it, and the process must end. Start-up took up to 13 s
# on CI runners in the spike, the first time WebView2 makes its profile.
add_test(NAME app_bridge_check COMMAND collabgod --bridge-check)
# It passes on its exit code alone: 0 only if the check passed and the app then quit.
set_tests_properties(app_bridge_check PROPERTIES TIMEOUT 180)

if(APPLE)
    # The deployment target set once in the root CMakeLists.txt reaches the binary.
    add_test(NAME app_macos_minimum
        COMMAND ${CMAKE_COMMAND}
            -DBINARY=$<TARGET_FILE:collabgod>
            -DEXPECTED=${CMAKE_OSX_DEPLOYMENT_TARGET}
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckMacosMinimum.cmake)
endif()
