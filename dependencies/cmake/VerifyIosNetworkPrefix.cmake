# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED PREFIX)
    message(FATAL_ERROR "PREFIX is required for dependency verification")
endif()
if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required for dependency verification")
endif()
if(NOT DEFINED PLATFORM)
    set(PLATFORM ios-simulator)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/VerifyManifestProduct.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/VerifyPrefix.cmake")
if(PLATFORM STREQUAL "ios-simulator")
    set(_platform_receipt ios-simulator-network-arm64.tsv)
    set(_mach_platform 7)
elseif(PLATFORM STREQUAL "ios")
    set(_platform_receipt ios-device-network-arm64.tsv)
    set(_mach_platform 2)
else()
    message(FATAL_ERROR "Unsupported iOS network platform: ${PLATFORM}")
endif()

set(_required
    "${PREFIX}/lib/libpcre2-8.a"
    "${PREFIX}/lib/libffi.a"
    "${PREFIX}/lib/libintl.a"
    "${PREFIX}/lib/libglib-2.0.a"
    "${PREFIX}/lib/libslirp.a"
    "${PREFIX}/include/pcre2.h"
    "${PREFIX}/include/ffi.h"
    "${PREFIX}/include/glib-2.0/glib.h"
    "${PREFIX}/lib/glib-2.0/include/glibconfig.h"
    "${PREFIX}/include/slirp/libslirp.h")
foreach(_file IN LISTS _required)
    if(NOT EXISTS "${_file}" OR IS_SYMLINK "${_file}")
        message(FATAL_ERROR "Invalid iOS network dependency: ${_file}")
    endif()
endforeach()

uml_verify_dependency_manifest_product(
    SOURCE "${SOURCE_ROOT}/dependencies/manifest.tsv"
    INSTALLED "${PREFIX}/share/uml-dependencies/manifest.tsv"
    PRODUCT ios-network
    LABEL "iOS network")
uml_verify_source_trees(
    INSTALLED "${PREFIX}/share/uml-dependencies/source-trees.tsv"
    LABEL "iOS network"
    SOURCES
        glib-ios "${SOURCE_ROOT}/dependencies/patches/glib-ios/series.tsv"
        libslirp "${SOURCE_ROOT}/dependencies/patches/libslirp/series.tsv")
set(_provenance_files "${_platform_receipt}")
foreach(_relative IN LISTS _provenance_files)
    set(_source "${SOURCE_ROOT}/dependencies/platforms/${_relative}")
    set(_installed "${PREFIX}/share/uml-dependencies/${_relative}")
    if(NOT EXISTS "${_installed}" OR IS_SYMLINK "${_installed}")
        message(FATAL_ERROR
            "Missing iOS network dependency provenance: ${_installed}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${_source}" "${_installed}"
        RESULT_VARIABLE _provenance_status)
    if(NOT _provenance_status EQUAL 0)
        message(FATAL_ERROR
            "iOS network dependency provenance does not match: ${_relative}")
    endif()
endforeach()

foreach(_archive IN ITEMS
        libpcre2-8.a libffi.a libintl.a libglib-2.0.a libslirp.a)
    set(_path "${PREFIX}/lib/${_archive}")
    execute_process(
        COMMAND lipo -archs "${_path}"
        RESULT_VARIABLE _lipo_status
        OUTPUT_VARIABLE _architectures
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _lipo_status EQUAL 0 OR NOT _architectures STREQUAL "arm64")
        message(FATAL_ERROR
            "${_path} is not an arm64-only Apple archive: ${_architectures}")
    endif()

    execute_process(
        COMMAND otool -l "${_path}"
        RESULT_VARIABLE _otool_status
        OUTPUT_VARIABLE _load_commands)
    if(NOT _otool_status EQUAL 0 OR
       NOT _load_commands MATCHES "platform ${_mach_platform}([^0-9]|$)" OR
       NOT _load_commands MATCHES "minos 26\\.0")
        message(FATAL_ERROR
            "${_path} is not a ${PLATFORM} 26.0 archive")
    endif()
endforeach()
