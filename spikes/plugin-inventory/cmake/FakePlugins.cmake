# Builds a fake VST3 bundle from test/fake_plugin/FakeFactory.cpp. Spike code (issue #14).
#
#   add_fake_vst3(<name> FOLDER <dir> MODE <mode> NAME <class name> VERSION <v>
#                 CID <w1> <w2> <w3> <w4> [MODULE_INFO])
#
# Produces <dir>/<name>.vst3 laid out as the VST3 bundle format says:
#   macOS:   <name>.vst3/Contents/MacOS/<name> and Info.plist (a CFBundle)
#   Windows: <name>.vst3/Contents/x86_64-win/<name>.vst3 (or arm64-win)
#   Linux:   <name>.vst3/Contents/x86_64-linux/<name>.so (or aarch64-linux)
# MODULE_INFO also writes Contents/Resources/moduleinfo.json describing it.

set(COLLABGOD_FAKE_PLUGIN_TARGETS "" CACHE INTERNAL "")

function(_collabgod_vst3_arch_folder out)
    if(WIN32)
        if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "ARM64" OR CMAKE_GENERATOR_PLATFORM MATCHES "ARM64")
            set(${out} arm64-win PARENT_SCOPE)
        else()
            set(${out} x86_64-win PARENT_SCOPE)
        endif()
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(${out} aarch64-linux PARENT_SCOPE)
    else()
        set(${out} x86_64-linux PARENT_SCOPE)
    endif()
endfunction()

function(add_fake_vst3 name)
    cmake_parse_arguments(A "MODULE_INFO" "FOLDER;MODE;NAME;VERSION" "CID" ${ARGN})
    list(LENGTH A_CID cid_words)
    if(NOT cid_words EQUAL 4)
        message(FATAL_ERROR "add_fake_vst3(${name}): CID needs four 32-bit words")
    endif()
    list(GET A_CID 0 c1)
    list(GET A_CID 1 c2)
    list(GET A_CID 2 c3)
    list(GET A_CID 3 c4)

    set(target fake_vst3_${name})
    string(MAKE_C_IDENTIFIER ${target} target)
    add_library(${target} MODULE ${CMAKE_CURRENT_SOURCE_DIR}/test/fake_plugin/FakeFactory.cpp)
    target_link_libraries(${target} PRIVATE vst3_interfaces)
    target_compile_definitions(${target} PRIVATE
        FAKE_MODE=${A_MODE}
        FAKE_NAME="${A_NAME}"
        FAKE_VERSION="${A_VERSION}"
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
        FAKE_CID1=${c1}u FAKE_CID2=${c2}u FAKE_CID3=${c3}u FAKE_CID4=${c4}u)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${name}
        PREFIX ""
        CXX_VISIBILITY_PRESET hidden
        # Keep import libraries and the like out of the bundle.
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/fake-plugin-archives)

    set(bundle ${A_FOLDER}/${name}.vst3)

    if(APPLE)
        # A generator expression in the output folder stops multi-config
        # generators from adding a Release/ or Debug/ level.
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION vst3
            LIBRARY_OUTPUT_DIRECTORY "$<1:${A_FOLDER}>"
            MACOSX_BUNDLE_GUI_IDENTIFIER org.collabgod.tests.${name}
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${A_VERSION}
            MACOSX_BUNDLE_BUNDLE_VERSION ${A_VERSION})
    else()
        _collabgod_vst3_arch_folder(arch)
        if(WIN32)
            set(suffix .vst3)
        else()
            set(suffix .so)
        endif()
        set_target_properties(${target} PROPERTIES
            SUFFIX ${suffix}
            LIBRARY_OUTPUT_DIRECTORY "$<1:${bundle}/Contents/${arch}>"
            RUNTIME_OUTPUT_DIRECTORY "$<1:${bundle}/Contents/${arch}>")
    endif()

    if(A_MODULE_INFO)
        string(REGEX REPLACE "^0x" "" h1 ${c1})
        string(REGEX REPLACE "^0x" "" h2 ${c2})
        string(REGEX REPLACE "^0x" "" h3 ${c3})
        string(REGEX REPLACE "^0x" "" h4 ${c4})
        string(TOUPPER "${h1}${h2}${h3}${h4}" cid_text)
        set(info ${CMAKE_BINARY_DIR}/fake-plugin-moduleinfo/${name}.json)
        file(WRITE ${info} "{
  \"Name\": \"${name}\",
  \"Version\": \"${A_VERSION}\",
  \"Factory Info\": { \"Vendor\": \"CollabGod Tests\", \"URL\": \"\", \"E-Mail\": \"\", \"Flags\": { \"Unicode\": true, \"Classes Discardable\": false, \"Component Non Discardable\": false, }, },
  \"Compatibility\": [ { \"New\": \"${cid_text}\", \"Old\": [ \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\", ], }, ],
  \"Classes\": [
    { \"CID\": \"${cid_text}\", \"Category\": \"Audio Module Class\", \"Name\": \"${A_NAME}\", \"Vendor\": \"CollabGod Tests\", \"Version\": \"${A_VERSION}\", \"SDKVersion\": \"VST 3.8.1\", \"Sub Categories\": [ \"Instrument\", \"Synth\", ], \"Class Flags\": 0, \"Cardinality\": 2147483647, \"Snapshots\": [ ], },
  ],
}
")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${bundle}/Contents/Resources
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${info} ${bundle}/Contents/Resources/moduleinfo.json)
    endif()

    set(COLLABGOD_FAKE_PLUGIN_TARGETS ${COLLABGOD_FAKE_PLUGIN_TARGETS} ${target} CACHE INTERNAL "")
endfunction()
