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
if(PLATFORM STREQUAL "ios-simulator")
    set(_platform_receipt ios-simulator-network-arm64.tsv)
    set(_mach_platform 7)
elseif(PLATFORM STREQUAL "ios")
    set(_platform_receipt ios-device-network-arm64.tsv)
    set(_mach_platform 2)
else()
    message(FATAL_ERROR "Unsupported Kernarium network platform: ${PLATFORM}")
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
    "${PREFIX}/include/slirp/libslirp.h"
    "${PREFIX}/share/kernarium/dependency-manifest.tsv"
    "${PREFIX}/share/kernarium/${_platform_receipt}"
    "${PREFIX}/share/kernarium/patches/glib/0001-ios-avoid-unavailable-process-apis.patch")
foreach(_file IN LISTS _required)
    if(NOT EXISTS "${_file}" OR IS_SYMLINK "${_file}")
        message(FATAL_ERROR
            "Invalid Kernarium iOS network dependency: ${_file}")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${SOURCE_ROOT}/dependencies/manifest.tsv"
        "${PREFIX}/share/kernarium/dependency-manifest.tsv"
    RESULT_VARIABLE _manifest_status)
if(NOT _manifest_status EQUAL 0)
    message(FATAL_ERROR
        "Kernarium iOS network source manifest does not match")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${SOURCE_ROOT}/Kernarium/dependencies/${_platform_receipt}"
        "${PREFIX}/share/kernarium/${_platform_receipt}"
    RESULT_VARIABLE _receipt_status)
if(NOT _receipt_status EQUAL 0)
    message(FATAL_ERROR
        "Kernarium iOS network platform receipt does not match")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${SOURCE_ROOT}/Kernarium/dependencies/patches/glib/0001-ios-avoid-unavailable-process-apis.patch"
        "${PREFIX}/share/kernarium/patches/glib/0001-ios-avoid-unavailable-process-apis.patch"
    RESULT_VARIABLE _patch_status)
if(NOT _patch_status EQUAL 0)
    message(FATAL_ERROR "Kernarium iOS GLib patch does not match")
endif()

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
       NOT _load_commands MATCHES "minos 16\\.0")
        message(FATAL_ERROR
            "${_path} is not a ${PLATFORM} 16.0 archive")
    endif()
endforeach()
