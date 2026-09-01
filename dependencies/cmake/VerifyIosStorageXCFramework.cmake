# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED PREFIX)
    message(FATAL_ERROR "PREFIX is required for storage verification")
endif()
if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required for storage verification")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/VerifyManifestProduct.cmake")

set(_xcframework "${PREFIX}/GPTFDisk.xcframework")
set(_required
    "${_xcframework}/Info.plist"
    "${_xcframework}/ios-arm64/GPTFDisk.framework/GPTFDisk"
    "${_xcframework}/ios-arm64/GPTFDisk.framework/Headers/GPTFDisk.h"
    "${_xcframework}/ios-arm64/GPTFDisk.framework/Modules/module.modulemap"
    "${_xcframework}/ios-arm64-simulator/GPTFDisk.framework/GPTFDisk"
    "${_xcframework}/ios-arm64-simulator/GPTFDisk.framework/Headers/GPTFDisk.h"
    "${_xcframework}/ios-arm64-simulator/GPTFDisk.framework/Modules/module.modulemap")
foreach(_file IN LISTS _required)
    if(NOT EXISTS "${_file}" OR IS_SYMLINK "${_file}")
        message(FATAL_ERROR "Invalid iOS storage dependency: ${_file}")
    endif()
endforeach()

uml_verify_dependency_manifest_product(
    SOURCE "${SOURCE_ROOT}/dependencies/manifest.tsv"
    INSTALLED "${PREFIX}/share/uml-dependencies/manifest.tsv"
    PRODUCT ios-storage
    LABEL "iOS storage")
set(_provenance_files
    "ios-device-storage-arm64.tsv"
    "ios-simulator-storage-arm64.tsv"
    "patches/gptfdisk/series.tsv")
file(STRINGS
    "${SOURCE_ROOT}/dependencies/patches/gptfdisk/series.tsv" _series_rows)
foreach(_row IN LISTS _series_rows)
    if(_row MATCHES "^#" OR _row STREQUAL "")
        continue()
    endif()
    string(REPLACE "\t" ";" _fields "${_row}")
    list(GET _fields 0 _order)
    if(NOT _order STREQUAL "order")
        list(GET _fields 1 _patch)
        list(APPEND _provenance_files "patches/gptfdisk/${_patch}")
    endif()
endforeach()
foreach(_relative IN LISTS _provenance_files)
    if(_relative MATCHES "^ios-.*-storage-arm64\\.tsv$")
        set(_source "${SOURCE_ROOT}/dependencies/platforms/${_relative}")
    else()
        set(_source "${SOURCE_ROOT}/dependencies/${_relative}")
    endif()
    set(_installed "${PREFIX}/share/uml-dependencies/${_relative}")
    if(NOT EXISTS "${_installed}" OR IS_SYMLINK "${_installed}")
        message(FATAL_ERROR
            "Missing iOS storage dependency provenance: ${_installed}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files "${_source}" "${_installed}"
        RESULT_VARIABLE _provenance_status)
    if(NOT _provenance_status EQUAL 0)
        message(FATAL_ERROR
            "iOS storage dependency provenance does not match: ${_relative}")
    endif()
endforeach()

foreach(_slice IN ITEMS ios-arm64 ios-arm64-simulator)
    set(_archive "${_xcframework}/${_slice}/GPTFDisk.framework/GPTFDisk")
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
       NOT _load_commands MATCHES "minos 16\\.0")
        message(FATAL_ERROR "${_archive} has the wrong Apple platform contract")
    endif()

    execute_process(
        COMMAND nm -g "${_archive}"
        RESULT_VARIABLE _nm_status
        OUTPUT_VARIABLE _symbols)
    if(NOT _nm_status EQUAL 0 OR
       NOT _symbols MATCHES "_kernarium_gptfdisk_read_partition" OR
       NOT _symbols MATCHES "_kernarium_gptfdisk_grow_partition_to_fill")
        message(FATAL_ERROR "${_archive} is missing the GPT core C API")
    endif()
    if(_symbols MATCHES "(^|[\n ])_main([\n ]|$)" OR
       _symbols MATCHES "GPTData(TextUI|CL|Curses)" OR
       _symbols MATCHES "[Pp]opt")
        message(FATAL_ERROR "${_archive} contains a GPT fdisk CLI front end")
    endif()
endforeach()
