# Patches to the downloaded Tracktion Engine source, applied at configure time.
# Included by Dependencies.cmake after the fetch.
#
# Apple clang 15 (Xcode 15, the newest on macOS 13 and 14) lacks C++20 parenthesised
# aggregate initialisation, which Tracktion Engine uses in two places. Requiring
# Xcode 16 was ruled out on 2026-11-22: it needs macOS 14, and neither maintainer has
# a Mac that can run it (#19, engine-toolchain README finding 11). So we carry two
# small patches until Tracktion fixes them upstream.
#
# They are applied on every system, not only on macOS, so that Windows and Linux
# compile the same source the Mac ships and a broken patch fails everywhere. Both
# mean exactly what the original code means.
#
# Upstream reports: not filed yet; draft text is on issue #19. When a
# report exists, put its link next to its patch below. When Tracktion fixes one,
# the patch stops matching, configure prints a warning, and it can be deleted.
#
# A file is written only when its text changes, so configuring again does not touch
# its timestamp and trigger a rebuild. A local checkout given with
# FETCHCONTENT_SOURCE_DIR_TRACKTION is never patched: it is the developer's own tree.

include_guard(GLOBAL)

function(collabgod_patch_source file old new)
    file(READ "${file}" text)
    string(FIND "${text}" "${new}" already)
    if(NOT already EQUAL -1)
        return()
    endif()
    string(FIND "${text}" "${old}" found)
    if(found EQUAL -1)
        message(WARNING "Tracktion Engine patch no longer matches ${file}. "
                        "Check whether upstream fixed it, and remove the patch from cmake/TracktionPatches.cmake if so.")
        return()
    endif()
    string(REPLACE "${old}" "${new}" text "${text}")
    file(WRITE "${file}" "${text}")
    message(STATUS "Patched ${file} for Apple clang 15")
endfunction()

if(FETCHCONTENT_SOURCE_DIR_TRACKTION)
    message(STATUS "Using a local Tracktion Engine checkout; not applying the Apple clang 15 patches")
    return()
endif()

set(_te "${tracktion_SOURCE_DIR}/modules/tracktion_engine")

# 1. ExportableUpdate (a, b) on an aggregate: brace-init means the same.
collabgod_patch_source("${_te}/model/export/tracktion_Archive.cpp"
    "ExportableUpdate (f.oldRef, destItem->getProjectItemRef())"
    "ExportableUpdate { f.oldRef, destItem->getProjectItemRef() }")

# 2. emplace_back (a, b) on the aggregate ChannelMap::Entry: give it a constructor.
#    emplace_back cannot brace-init, so the struct gets one instead.
collabgod_patch_source("${_te}/utilities/tracktion_ChannelMap.h"
    "        float gain = 1.0f;\n    };"
    "        float gain = 1.0f;\n\n        Entry() = default;\n        Entry (int s, int d, float g = 1.0f) : source (s), dest (d), gain (g) {}\n    };")
