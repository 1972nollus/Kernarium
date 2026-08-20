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
    set(_platform_receipt ios-simulator-gpu-arm64.tsv)
    set(_mach_platform 7)
elseif(PLATFORM STREQUAL "ios")
    set(_platform_receipt ios-device-gpu-arm64.tsv)
    set(_mach_platform 2)
else()
    message(FATAL_ERROR "Unsupported iOS GPU platform: ${PLATFORM}")
endif()

set(_provenance_files
    "manifest.tsv"
    "${_platform_receipt}")
foreach(_series IN ITEMS
        "patches/utm-webkit/series.tsv"
        "patches/utm-virglrenderer/series.tsv")
    list(APPEND _provenance_files "${_series}")
    get_filename_component(_series_directory "${_series}" DIRECTORY)
    file(STRINGS "${SOURCE_ROOT}/dependencies/${_series}" _series_rows)
    foreach(_row IN LISTS _series_rows)
        if(_row MATCHES "^#" OR _row STREQUAL "")
            continue()
        endif()
        string(REPLACE "\t" ";" _fields "${_row}")
        list(GET _fields 0 _order)
        if(NOT _order STREQUAL "order")
            list(GET _fields 1 _patch)
            list(APPEND _provenance_files
                "${_series_directory}/${_patch}")
        endif()
    endforeach()
endforeach()
set(_required
    "${PREFIX}/lib/libepoxy.a"
    "${PREFIX}/lib/libvirglrenderer.a"
    "${PREFIX}/include/epoxy/egl.h"
    "${PREFIX}/include/virgl/virglrenderer.h"
    "${PREFIX}/include/virgl/virgl_hw.h"
    "${PREFIX}/Frameworks/EGL.framework/EGL"
    "${PREFIX}/Frameworks/GLESv2.framework/GLESv2"
    "${PREFIX}/Frameworks/EGL.framework/Info.plist"
    "${PREFIX}/Frameworks/GLESv2.framework/Info.plist")
foreach(_file IN LISTS _required)
    if(NOT EXISTS "${_file}" OR IS_SYMLINK "${_file}")
        message(FATAL_ERROR "Invalid iOS GPU dependency: ${_file}")
    endif()
endforeach()

foreach(_relative IN LISTS _provenance_files)
    if(_relative STREQUAL "${_platform_receipt}")
        set(_source "${SOURCE_ROOT}/dependencies/platforms/${_relative}")
    else()
        set(_source "${SOURCE_ROOT}/dependencies/${_relative}")
    endif()
    set(_installed "${PREFIX}/share/uml-dependencies/${_relative}")
    if(NOT EXISTS "${_installed}" OR IS_SYMLINK "${_installed}")
        message(FATAL_ERROR
            "Missing iOS GPU dependency provenance: ${_installed}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${_source}" "${_installed}"
        RESULT_VARIABLE _provenance_status)
    if(NOT _provenance_status EQUAL 0)
        message(FATAL_ERROR
            "iOS GPU dependency provenance does not match: ${_relative}")
    endif()
endforeach()

foreach(_binary IN ITEMS
        "${PREFIX}/lib/libepoxy.a"
        "${PREFIX}/lib/libvirglrenderer.a"
        "${PREFIX}/Frameworks/EGL.framework/EGL"
        "${PREFIX}/Frameworks/GLESv2.framework/GLESv2")
    execute_process(
        COMMAND lipo -archs "${_binary}"
        RESULT_VARIABLE _status
        OUTPUT_VARIABLE _architectures
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _status EQUAL 0 OR NOT _architectures STREQUAL "arm64")
        message(FATAL_ERROR
            "${_binary} is not an arm64-only Apple binary: ${_architectures}")
    endif()

    execute_process(
        COMMAND otool -l "${_binary}"
        RESULT_VARIABLE _otool_status
        OUTPUT_VARIABLE _load_commands)
    if(NOT _otool_status EQUAL 0 OR
       NOT _load_commands MATCHES "platform ${_mach_platform}([^0-9]|$)" OR
       NOT _load_commands MATCHES "minos 16\\.0")
        message(FATAL_ERROR
            "${_binary} is not a ${PLATFORM} 16.0 binary")
    endif()
endforeach()

foreach(_framework IN ITEMS EGL GLESv2)
    execute_process(
        COMMAND otool -D
            "${PREFIX}/Frameworks/${_framework}.framework/${_framework}"
        RESULT_VARIABLE _id_status
        OUTPUT_VARIABLE _install_name_output)
    if(NOT _id_status EQUAL 0 OR NOT _install_name_output MATCHES
       "@rpath/${_framework}\\.framework/${_framework}")
        message(FATAL_ERROR
            "${_framework}.framework has an invalid install name")
    endif()

    execute_process(
        COMMAND plutil -extract MinimumOSVersion raw -o -
            "${PREFIX}/Frameworks/${_framework}.framework/Info.plist"
        RESULT_VARIABLE _plist_status
        OUTPUT_VARIABLE _minimum_os
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _plist_status EQUAL 0 OR NOT _minimum_os STREQUAL "16.0")
        message(FATAL_ERROR
            "${_framework}.framework has an invalid deployment target")
    endif()
endforeach()
