# Spike for issue #14: VST3 plugin inventory and identity across two machines.
# Throwaway code. See README.md for what it found and how to run it.
#
# The scanner needs only Steinberg's VST3 interface headers (MIT), fetched at
# configure time and pinned to a commit like the engine spikes' dependencies
# (ADR-0009). No JUCE or Tracktion Engine, so it builds in seconds.

cmake_minimum_required(VERSION 3.22)

project(CollabGodPluginInventorySpike VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Oldest macOS the spike runs on")
endif()

# ---------------------------------------------------------------------------
# Steinberg's VST3 interfaces, v3.8.1 build 84. Headers only.
# ---------------------------------------------------------------------------

set(COLLABGOD_VST3_INTERFACES_COMMIT 4f547e8e102b47de4a8b8aaf343c73b700786372) # v3.8.1_build_84, 2026-08-11

include(FetchContent)
FetchContent_Declare(vst3_pluginterfaces
    DOWNLOAD_COMMAND
        ${CMAKE_COMMAND}
            -DREPO=https://github.com/steinbergmedia/vst3_pluginterfaces.git
            -DCOMMIT=${COLLABGOD_VST3_INTERFACES_COMMIT}
            -DDEST=<SOURCE_DIR>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/../engine-toolchain/cmake/ShallowFetchCommit.cmake
    SOURCE_SUBDIR do-not-configure)
FetchContent_MakeAvailable(vst3_pluginterfaces)

# The headers include each other as "pluginterfaces/base/...", so the include
# root is the folder above the checkout, seen under that name.
set(_vst3_include_root ${CMAKE_BINARY_DIR}/vst3-include)
file(MAKE_DIRECTORY ${_vst3_include_root})
if(NOT EXISTS ${_vst3_include_root}/pluginterfaces/base/ipluginbase.h)
    file(COPY ${vst3_pluginterfaces_SOURCE_DIR}/base ${vst3_pluginterfaces_SOURCE_DIR}/vst
         ${vst3_pluginterfaces_SOURCE_DIR}/gui ${vst3_pluginterfaces_SOURCE_DIR}/LICENSE.txt
         DESTINATION ${_vst3_include_root}/pluginterfaces)
endif()

add_library(vst3_interfaces INTERFACE)
target_include_directories(vst3_interfaces SYSTEM INTERFACE ${_vst3_include_root})

if(MSVC)
    set(_warnings /W4 /permissive- /utf-8)
else()
    set(_warnings -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

# ---------------------------------------------------------------------------
# The scanner.
# ---------------------------------------------------------------------------

add_executable(plugin_scan
    scanner/ChildProcess.cpp
    scanner/ChildProcess.h
    scanner/Discover.cpp
    scanner/Discover.h
    scanner/Json.cpp
    scanner/Json.h
    scanner/Main.cpp
    scanner/Platform.cpp
    scanner/Platform.h
    scanner/Probe.cpp
    scanner/Probe.h)

target_link_libraries(plugin_scan PRIVATE vst3_interfaces)
target_compile_options(plugin_scan PRIVATE ${_warnings})

if(WIN32)
    target_compile_definitions(plugin_scan PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_link_libraries(plugin_scan PRIVATE version ole32 shell32)
elseif(APPLE)
    target_link_libraries(plugin_scan PRIVATE "-framework CoreFoundation")
else()
    target_link_libraries(plugin_scan PRIVATE ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# Tests: fake plugins that behave well and badly, scanned for real.
# ---------------------------------------------------------------------------

include(cmake/FakePlugins.cmake)

set(_fixtures ${CMAKE_BINARY_DIR}/fixtures)

# One class ID for the well-behaved plugin, written as four 32-bit words. The
# test expects exactly this text on every system, which is the check that the
# scanner turns Windows' GUID byte order back into the canonical form.
set(_good_cid 0x1A2B3C4D 0x5E6F7081 0x92A3B4C5 0xD6E7F809)

add_fake_vst3(good            FOLDER ${_fixtures}/scan MODE normal          NAME "CollabGod Test Synth" VERSION 1.0.0 CID ${_good_cid} MODULE_INFO)
add_fake_vst3(nested          FOLDER ${_fixtures}/scan/Vendor/Sub MODE normal NAME "CollabGod Nested"   VERSION 2.1   CID 0x11111111 0x22222222 0x33333333 0x44444444)
add_fake_vst3(factory-v1      FOLDER ${_fixtures}/scan MODE factory_v1      NAME "CollabGod Old"        VERSION 0.9   CID 0x0BADF00D 0x00000001 0x00000002 0x00000003)
add_fake_vst3(crash-on-load   FOLDER ${_fixtures}/scan MODE crash_on_load   NAME "Crasher"              VERSION 1.0   CID 0xC0000000 0 0 1)
add_fake_vst3(hang-on-load    FOLDER ${_fixtures}/scan MODE hang_on_load    NAME "Hanger"               VERSION 1.0   CID 0xC0000000 0 0 2)
add_fake_vst3(crash-on-unload FOLDER ${_fixtures}/scan MODE crash_on_unload NAME "Unload Crasher"       VERSION 1.0   CID 0xC0000000 0 0 3)
add_fake_vst3(null-factory    FOLDER ${_fixtures}/scan MODE null_factory    NAME "Null"                 VERSION 1.0   CID 0xC0000000 0 0 4)
add_fake_vst3(no-export       FOLDER ${_fixtures}/scan MODE no_export       NAME "No Export"            VERSION 1.0   CID 0xC0000000 0 0 5)

# A bundle folder with no binary in it.
file(MAKE_DIRECTORY ${_fixtures}/scan/empty.vst3/Contents/Resources)

add_custom_target(fake_plugins ALL DEPENDS ${COLLABGOD_FAKE_PLUGIN_TARGETS})

enable_testing()

add_test(NAME scan_fake_plugins
    COMMAND ${CMAKE_COMMAND}
        -DSCANNER=$<TARGET_FILE:plugin_scan>
        -DFIXTURES=${_fixtures}/scan
        -DOUT=${CMAKE_BINARY_DIR}/fixtures-inventory.json
        -DGOOD_CID=1A2B3C4D5E6F708192A3B4C5D6E7F809
        -P ${CMAKE_CURRENT_SOURCE_DIR}/test/CheckInventory.cmake)
set_tests_properties(scan_fake_plugins PROPERTIES TIMEOUT 120)
