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
if(PLATFORM STREQUAL "ios-simulator")
    set(_platform_receipt ios-simulator-x86-arm64.tsv)
    set(_mach_platform 7)
elseif(PLATFORM STREQUAL "ios")
    set(_platform_receipt ios-device-x86-arm64.tsv)
    set(_mach_platform 2)
else()
    message(FATAL_ERROR "Unsupported iOS x86 support platform: ${PLATFORM}")
endif()

set(_required
    "${PREFIX}/lib/libsoftfloat_3e.a"
    "${PREFIX}/lib/libcephes_128bit.a"
    "${PREFIX}/include/SoftFloat-3e/softfloat.h"
    "${PREFIX}/include/SoftFloat-3e/softfloat_types.h"
    "${PREFIX}/include/cephes_128bit.h"
    "${PREFIX}/share/uml-dependencies/manifest.tsv"
    "${PREFIX}/share/uml-dependencies/${_platform_receipt}")
foreach(_file IN LISTS _required)
    if(NOT EXISTS "${_file}" OR IS_SYMLINK "${_file}")
        message(FATAL_ERROR "Invalid iOS x86 support dependency: ${_file}")
    endif()
endforeach()

uml_verify_dependency_manifest_product(
    SOURCE "${SOURCE_ROOT}/dependencies/manifest.tsv"
    INSTALLED "${PREFIX}/share/uml-dependencies/manifest.tsv"
    PRODUCT ios-x86
    LABEL "iOS x86 support")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${SOURCE_ROOT}/dependencies/platforms/${_platform_receipt}"
        "${PREFIX}/share/uml-dependencies/${_platform_receipt}"
    RESULT_VARIABLE _receipt_status)
if(NOT _receipt_status EQUAL 0)
    message(FATAL_ERROR "iOS x86 support platform receipt does not match")
endif()

foreach(_archive IN ITEMS libsoftfloat_3e.a libcephes_128bit.a)
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
        message(FATAL_ERROR "${_path} is not a ${PLATFORM} 26.0 archive")
    endif()
endforeach()

file(GLOB _archives "${PREFIX}/lib/*.a")
list(LENGTH _archives _archive_count)
if(NOT _archive_count EQUAL 2)
    message(FATAL_ERROR "iOS x86 support prefix contains undeclared archives")
endif()
