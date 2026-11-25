# Test: the built binary says it needs the macOS version the build asked for, and no
# newer (#145). Run by ctest on macOS:
#
#   cmake -DBINARY=<path> -DEXPECTED=<version> -P CheckMacosMinimum.cmake
#
# Reads the binary's load commands with otool: LC_BUILD_VERSION ("minos 12.0"), or
# LC_VERSION_MIN_MACOSX ("version 12.0") from older linkers. A universal binary has
# one per architecture, and every one must match: `-arch all` makes otool print
# them all, where by default it reads only this machine's architecture.

foreach(var BINARY EXPECTED)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "CheckMacosMinimum.cmake: ${var} is required")
    endif()
endforeach()

execute_process(
    COMMAND otool -arch all -l "${BINARY}"
    OUTPUT_VARIABLE load_commands
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "otool could not read ${BINARY}")
endif()

string(REGEX MATCHALL "(minos|cmd LC_VERSION_MIN_MACOSX[^\n]*\n[^\n]*\n[ ]*version) [0-9.]+" found "${load_commands}")
if(NOT found)
    message(FATAL_ERROR "No minimum macOS version in ${BINARY}")
endif()

foreach(entry IN LISTS found)
    string(REGEX MATCH "[0-9.]+$" version "${entry}")
    if(NOT version VERSION_EQUAL EXPECTED)
        message(FATAL_ERROR "${BINARY} needs macOS ${version}; the build asked for ${EXPECTED}")
    endif()
    message(STATUS "Minimum macOS ${version}: as asked for")
endforeach()
