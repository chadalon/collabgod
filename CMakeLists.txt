# CollabGod: the C++ side of the build (the app, its engine and its tests).
#
# You normally do not run this directly: `npm run build` at the repository root
# configures and builds it along with the TypeScript packages. See README.md.
#
# Directly:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release --parallel 2
#   ctest --test-dir build -C Release --output-on-failure

cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------
# macOS: the oldest version the app runs on (#145). This is the only place it is
# set. It must be set before project(), and it goes into the app's Info.plist.
# ---------------------------------------------------------------------------

set(COLLABGOD_MACOS_MINIMUM 12.0)

if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET ${COLLABGOD_MACOS_MINIMUM} CACHE STRING "Oldest macOS the app runs on")
endif()

project(CollabGod VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS COLLABGOD_MACOS_MINIMUM)
    message(FATAL_ERROR "CMAKE_OSX_DEPLOYMENT_TARGET is ${CMAKE_OSX_DEPLOYMENT_TARGET}; "
                        "CollabGod needs macOS ${COLLABGOD_MACOS_MINIMUM} or newer (#145)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    # Static runtime, as Tracktion Engine's own examples and the spikes use.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# JUCE's link-time optimisation made every rebuild of a one-line change take about
# 4 minutes to link, against 13 seconds without it (engine-toolchain README finding
# 6). It is off unless asked for, which only a release package should do.
option(COLLABGOD_LTO "Link with JUCE's link-time optimisation flags (slow links; for release packages only)" OFF)

# ---------------------------------------------------------------------------
# Dependencies: JUCE, Tracktion Engine and, on Windows, the WebView2 SDK.
# ---------------------------------------------------------------------------

include(cmake/Dependencies.cmake)

# ---------------------------------------------------------------------------
# The product. Nothing here may include or import anything under spikes/;
# scripts/check-no-spike-imports.mjs enforces that in `npm test`.
# ---------------------------------------------------------------------------

enable_testing()

add_subdirectory(app)
