# SPDX-License-Identifier: GPL-2.0-or-later
#
# Verifies an iOS static XCFramework prefix with one arm64 device and one
# arm64 Simulator slice, as dependencies/ios-static-xcframework.sh builds it.
# The caller's script is run with PREFIX and SOURCE_ROOT defined.
#
# uml_verify_ios_static_xcframework(
#     LABEL <text> PRODUCT <manifest product> FRAMEWORK <name>
#     HEADERS <header>... SYMBOLS <exported symbol>...
#     FORBIDDEN <regex>... FORBIDDEN_MESSAGE <text>
#     [SOURCE_TREES <source> <series.tsv>...])

include("${CMAKE_CURRENT_LIST_DIR}/VerifyManifestProduct.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/VerifySourceTrees.cmake")

function(uml_verify_ios_static_xcframework)
    cmake_parse_arguments(PARSE_ARGV 0 _arg ""
        "LABEL;PRODUCT;FRAMEWORK;FORBIDDEN_MESSAGE"
        "HEADERS;SYMBOLS;FORBIDDEN;SOURCE_TREES")
    if(NOT DEFINED PREFIX)
        message(FATAL_ERROR "PREFIX is required for ${_arg_LABEL} verification")
    endif()
    if(NOT DEFINED SOURCE_ROOT)
        message(FATAL_ERROR
            "SOURCE_ROOT is required for ${_arg_LABEL} verification")
    endif()

    set(_xcframework "${PREFIX}/${_arg_FRAMEWORK}.xcframework")
    set(_required "${_xcframework}/Info.plist")
    foreach(_slice IN ITEMS ios-arm64 ios-arm64-simulator)
        set(_framework "${_xcframework}/${_slice}/${_arg_FRAMEWORK}.framework")
        list(APPEND _required "${_framework}/${_arg_FRAMEWORK}")
        foreach(_header IN LISTS _arg_HEADERS)
            list(APPEND _required "${_framework}/Headers/${_header}")
        endforeach()
        list(APPEND _required "${_framework}/Modules/module.modulemap")
    endforeach()
    foreach(_file IN LISTS _required)
        if(NOT EXISTS "${_file}" OR IS_SYMLINK "${_file}")
            message(FATAL_ERROR "Invalid ${_arg_LABEL} dependency: ${_file}")
        endif()
    endforeach()

    uml_verify_dependency_manifest_product(
        SOURCE "${SOURCE_ROOT}/dependencies/manifest.tsv"
        INSTALLED "${PREFIX}/share/uml-dependencies/manifest.tsv"
        PRODUCT ${_arg_PRODUCT}
        LABEL "${_arg_LABEL}")
    if(_arg_SOURCE_TREES)
        uml_verify_source_trees(
            INSTALLED "${PREFIX}/share/uml-dependencies/source-trees.tsv"
            LABEL "${_arg_LABEL}"
            SOURCES ${_arg_SOURCE_TREES})
    endif()
    string(REGEX REPLACE "^ios-" "" _contract "${_arg_PRODUCT}")
    foreach(_relative IN ITEMS
            "ios-device-${_contract}-arm64.tsv"
            "ios-simulator-${_contract}-arm64.tsv")
        set(_source "${SOURCE_ROOT}/dependencies/platforms/${_relative}")
        set(_installed "${PREFIX}/share/uml-dependencies/${_relative}")
        if(NOT EXISTS "${_installed}" OR IS_SYMLINK "${_installed}")
            message(FATAL_ERROR
                "Missing ${_arg_LABEL} dependency provenance: ${_installed}")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E compare_files
                "${_source}" "${_installed}"
            RESULT_VARIABLE _provenance_status)
        if(NOT _provenance_status EQUAL 0)
            message(FATAL_ERROR
                "${_arg_LABEL} dependency provenance does not match: ${_relative}")
        endif()
    endforeach()

    foreach(_slice IN ITEMS ios-arm64 ios-arm64-simulator)
        set(_archive
            "${_xcframework}/${_slice}/${_arg_FRAMEWORK}.framework/${_arg_FRAMEWORK}")
        execute_process(
            COMMAND lipo -archs "${_archive}"
            RESULT_VARIABLE _lipo_status
            OUTPUT_VARIABLE _architectures
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT _lipo_status EQUAL 0 OR NOT _architectures STREQUAL "arm64")
            message(FATAL_ERROR
                "${_archive} is not an arm64-only Apple archive: ${_architectures}")
        endif()

        if(_slice STREQUAL "ios-arm64")
            set(_mach_platform 2)
        else()
            set(_mach_platform 7)
        endif()
        execute_process(
            COMMAND otool -l "${_archive}"
            RESULT_VARIABLE _otool_status
            OUTPUT_VARIABLE _load_commands)
        if(NOT _otool_status EQUAL 0 OR
           NOT _load_commands MATCHES "platform ${_mach_platform}([^0-9]|$)" OR
           NOT _load_commands MATCHES "minos 26\\.0")
            message(FATAL_ERROR
                "${_archive} has the wrong Apple platform contract")
        endif()

        execute_process(
            COMMAND nm -g "${_archive}"
            RESULT_VARIABLE _nm_status
            OUTPUT_VARIABLE _symbols)
        if(NOT _nm_status EQUAL 0)
            message(FATAL_ERROR
                "Cannot inspect ${_arg_LABEL} symbols in ${_archive}")
        endif()
        foreach(_symbol IN LISTS _arg_SYMBOLS)
            if(NOT _symbols MATCHES "_${_symbol}([^A-Za-z0-9_]|$)")
                message(FATAL_ERROR "${_archive} is missing ${_symbol}")
            endif()
        endforeach()
        foreach(_pattern IN LISTS _arg_FORBIDDEN)
            if(_symbols MATCHES "${_pattern}")
                message(FATAL_ERROR
                    "${_archive} contains ${_arg_FORBIDDEN_MESSAGE}")
            endif()
        endforeach()
    endforeach()
endfunction()
