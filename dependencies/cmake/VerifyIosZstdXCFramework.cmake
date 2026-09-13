# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED PREFIX)
    message(FATAL_ERROR "PREFIX is required for zstd verification")
endif()
if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required for zstd verification")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/VerifyManifestProduct.cmake")

set(_xcframework "${PREFIX}/LibZstd.xcframework")
set(_required
    "${_xcframework}/Info.plist"
    "${_xcframework}/ios-arm64/LibZstd.framework/LibZstd"
    "${_xcframework}/ios-arm64/LibZstd.framework/Headers/zstd.h"
    "${_xcframework}/ios-arm64/LibZstd.framework/Headers/zstd_errors.h"
    "${_xcframework}/ios-arm64/LibZstd.framework/Modules/module.modulemap"
    "${_xcframework}/ios-arm64-simulator/LibZstd.framework/LibZstd"
    "${_xcframework}/ios-arm64-simulator/LibZstd.framework/Headers/zstd.h"
    "${_xcframework}/ios-arm64-simulator/LibZstd.framework/Headers/zstd_errors.h"
    "${_xcframework}/ios-arm64-simulator/LibZstd.framework/Modules/module.modulemap")
foreach(_file IN LISTS _required)
    if(NOT EXISTS "${_file}" OR IS_SYMLINK "${_file}")
        message(FATAL_ERROR "Invalid iOS zstd dependency: ${_file}")
    endif()
endforeach()

uml_verify_dependency_manifest_product(
    SOURCE "${SOURCE_ROOT}/dependencies/manifest.tsv"
    INSTALLED "${PREFIX}/share/uml-dependencies/manifest.tsv"
    PRODUCT ios-compression
    LABEL "iOS zstd")
set(_provenance_files
    "ios-device-compression-arm64.tsv"
    "ios-simulator-compression-arm64.tsv")
foreach(_relative IN LISTS _provenance_files)
    if(_relative MATCHES "^ios-.*-compression-arm64\\.tsv$")
        set(_source "${SOURCE_ROOT}/dependencies/platforms/${_relative}")
    else()
        set(_source "${SOURCE_ROOT}/dependencies/${_relative}")
    endif()
    set(_installed "${PREFIX}/share/uml-dependencies/${_relative}")
    if(NOT EXISTS "${_installed}" OR IS_SYMLINK "${_installed}")
        message(FATAL_ERROR
            "Missing iOS zstd dependency provenance: ${_installed}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files "${_source}" "${_installed}"
        RESULT_VARIABLE _provenance_status)
    if(NOT _provenance_status EQUAL 0)
        message(FATAL_ERROR
            "iOS zstd dependency provenance does not match: ${_relative}")
    endif()
endforeach()

foreach(_slice IN ITEMS ios-arm64 ios-arm64-simulator)
    set(_archive "${_xcframework}/${_slice}/LibZstd.framework/LibZstd")
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
        message(FATAL_ERROR "${_archive} has the wrong Apple platform contract")
    endif()

    execute_process(
        COMMAND nm -g "${_archive}"
        RESULT_VARIABLE _nm_status
        OUTPUT_VARIABLE _symbols)
    if(NOT _nm_status EQUAL 0)
        message(FATAL_ERROR "Cannot inspect zstd symbols in ${_archive}")
    endif()
    foreach(_symbol IN ITEMS
            ZSTD_createDStream
            ZSTD_DStreamInSize
            ZSTD_DStreamOutSize
            ZSTD_decompressStream
            ZSTD_freeDStream
            ZSTD_getErrorName
            ZSTD_initDStream
            ZSTD_isError
            ZSTD_versionNumber)
        if(NOT _symbols MATCHES "_${_symbol}([^A-Za-z0-9_]|$)")
            message(FATAL_ERROR "${_archive} is missing ${_symbol}")
        endif()
    endforeach()
    if(_symbols MATCHES "(^|[\n ])_main([\n ]|$)" OR
       _symbols MATCHES "_ZSTD_compress(Stream|CCtx|2)?([^A-Za-z0-9_]|$)" OR
       _symbols MATCHES "_ZDICT_trainFromBuffer" OR
       _symbols MATCHES "_ZSTDv0[1-7]_")
        message(FATAL_ERROR
            "${_archive} contains a zstd CLI or unused compression API")
    endif()
endforeach()
