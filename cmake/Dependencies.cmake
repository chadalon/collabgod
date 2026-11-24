# The app's C++ dependencies: JUCE 9 and Tracktion Engine (ADR-0002), and on Windows
# the WebView2 SDK.
#
# JUCE and Tracktion Engine are fetched at configure time, each at one full commit
# hash with no history (ADR-0009). Afterwards the targets tracktion::tracktion_engine,
# juce::juce_gui_extra and the rest exist. Every pin is also listed in THIRD_PARTY.md;
# change both together.
#
# To build against a local checkout instead (to debug or patch a dependency),
# configure with -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE or
# -DFETCHCONTENT_SOURCE_DIR_TRACKTION=/path/to/tracktion_engine. A local checkout is
# never patched. After the first configure, -DFETCHCONTENT_FULLY_DISCONNECTED=ON
# configures again without the network.

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# Pins. JUCE is the exact commit Tracktion Engine's own `modules/juce` submodule
# points at, so we build the pair Tracktion tests together. When you move
# Tracktion Engine, move JUCE to its new submodule commit in the same change:
#   git ls-tree <tracktion-commit> modules/juce
# Moving a pin is a deliberate change with CI on Windows and macOS (ADR-0009).
# ---------------------------------------------------------------------------

set(COLLABGOD_TRACKTION_COMMIT a5e44996f5f08dfa6af5f5d02d900fb3cfa37422) # 3.5.0 on develop, 2026-10-24
set(COLLABGOD_JUCE_COMMIT      7ff644f65d7daa1428c34b9738ebb5c449dc9172) # JUCE 9.0.5 + develop, 2026-10-16

include(FetchContent)

set(_collabgod_cmake_dir "${CMAKE_CURRENT_LIST_DIR}")

# Declares a dependency fetched at one commit with no history. See
# ShallowFetchCommit.cmake for why GIT_TAG + GIT_SHALLOW is not used.
function(collabgod_declare_pinned name repo commit)
    FetchContent_Declare(${name}
        DOWNLOAD_COMMAND
            ${CMAKE_COMMAND}
                -DREPO=${repo}
                -DCOMMIT=${commit}
                -DDEST=<SOURCE_DIR>
                -P ${_collabgod_cmake_dir}/ShallowFetchCommit.cmake
        ${ARGN})
endfunction()

collabgod_declare_pinned(juce
    https://github.com/juce-framework/JUCE.git
    ${COLLABGOD_JUCE_COMMIT})

# Tracktion Engine's top-level CMakeLists builds its examples and adds JUCE from
# its own submodule. We want neither: submodules are never fetched, and
# SOURCE_SUBDIR points at a folder with no CMakeLists.txt, so FetchContent only
# downloads it. The modules folder is added below.
collabgod_declare_pinned(tracktion
    https://github.com/Tracktion/tracktion_engine.git
    ${COLLABGOD_TRACKTION_COMMIT}
    SOURCE_SUBDIR do-not-configure)

FetchContent_MakeAvailable(juce tracktion)

include(${_collabgod_cmake_dir}/TracktionPatches.cmake)

add_subdirectory(${tracktion_SOURCE_DIR}/modules ${CMAKE_BINARY_DIR}/tracktion_modules)

if(WIN32)
    include(${_collabgod_cmake_dir}/WebView2.cmake)
endif()
