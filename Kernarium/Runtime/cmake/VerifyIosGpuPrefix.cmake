# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED PREFIX)
    message(FATAL_ERROR "PREFIX is required for dependency verification")
endif()
if(NOT DEFINED PLATFORM)
    set(PLATFORM ios-simulator)
endif()
if(PLATFORM STREQUAL "ios-simulator")
    set(_platform_receipt ios-simulator-arm64.tsv)
    set(_mach_platform 7)
elseif(PLATFORM STREQUAL "ios")
    set(_platform_receipt ios-device-arm64.tsv)
    set(_mach_platform 2)
else()
    message(FATAL_ERROR "Unsupported Kernarium GPU platform: ${PLATFORM}")
endif()

set(_required
    "${PREFIX}/lib/libepoxy.a"
    "${PREFIX}/lib/libvirglrenderer.a"
    "${PREFIX}/include/epoxy/egl.h"
    "${PREFIX}/include/virgl/virglrenderer.h"
    "${PREFIX}/include/virgl/virgl_hw.h"
    "${PREFIX}/Frameworks/EGL.framework/EGL"
    "${PREFIX}/Frameworks/GLESv2.framework/GLESv2"
    "${PREFIX}/Frameworks/EGL.framework/Info.plist"
    "${PREFIX}/Frameworks/GLESv2.framework/Info.plist"
    "${PREFIX}/share/kernarium/utm-gpu-sources.tsv"
    "${PREFIX}/share/kernarium/${_platform_receipt}"
    "${PREFIX}/share/kernarium/patches/angle/0001-fix-ios-simulator-varying-caps.patch"
    "${PREFIX}/share/kernarium/patches/virglrenderer/0001-ios-gles3-multisample-renderbuffers.patch")
foreach(_file IN LISTS _required)
    if(NOT EXISTS "${_file}" OR IS_SYMLINK "${_file}")
        message(FATAL_ERROR "Invalid Kernarium iOS GPU dependency: ${_file}")
    endif()
endforeach()

if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required for dependency verification")
endif()
foreach(_receipt IN ITEMS utm-gpu-sources.tsv ${_platform_receipt})
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${SOURCE_ROOT}/Kernarium/dependencies/${_receipt}"
            "${PREFIX}/share/kernarium/${_receipt}"
        RESULT_VARIABLE _receipt_status)
    if(NOT _receipt_status EQUAL 0)
        message(FATAL_ERROR
            "Kernarium iOS GPU receipt does not match: ${_receipt}")
    endif()
endforeach()

set(_angle_patch_relative
    "Kernarium/dependencies/patches/angle/0001-fix-ios-simulator-varying-caps.patch")
set(_installed_angle_patch
    "${PREFIX}/share/kernarium/patches/angle/0001-fix-ios-simulator-varying-caps.patch")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${SOURCE_ROOT}/${_angle_patch_relative}"
        "${_installed_angle_patch}"
    RESULT_VARIABLE _angle_patch_status)
if(NOT _angle_patch_status EQUAL 0)
    message(FATAL_ERROR
        "Kernarium iOS ANGLE patch does not match its source")
endif()

set(_virgl_patch_relative
    "Kernarium/dependencies/patches/virglrenderer/0001-ios-gles3-multisample-renderbuffers.patch")
set(_installed_virgl_patch
    "${PREFIX}/share/kernarium/patches/virglrenderer/0001-ios-gles3-multisample-renderbuffers.patch")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${SOURCE_ROOT}/${_virgl_patch_relative}"
        "${_installed_virgl_patch}"
    RESULT_VARIABLE _patch_status)
if(NOT _patch_status EQUAL 0)
    message(FATAL_ERROR
        "Kernarium iOS virglrenderer patch does not match its source")
endif()

file(STRINGS
    "${SOURCE_ROOT}/Kernarium/dependencies/${_platform_receipt}"
    _platform_receipt_lines)
set(_expected_patch_sha256 "")
set(_expected_angle_patch_sha256 "")
foreach(_line IN LISTS _platform_receipt_lines)
    if(_line MATCHES
       "^angle_patch_sha256[^0-9a-f]+([0-9a-f]+)$")
        set(_expected_angle_patch_sha256 "${CMAKE_MATCH_1}")
    endif()
    if(_line MATCHES
       "^virglrenderer_patch_sha256[^0-9a-f]+([0-9a-f]+)$")
        set(_expected_patch_sha256 "${CMAKE_MATCH_1}")
    endif()
endforeach()
string(LENGTH "${_expected_angle_patch_sha256}"
    _expected_angle_patch_sha256_length)
if(NOT _expected_angle_patch_sha256_length EQUAL 64)
    message(FATAL_ERROR
        "Kernarium iOS platform receipt lacks the ANGLE patch digest")
endif()
file(SHA256 "${_installed_angle_patch}" _actual_angle_patch_sha256)
if(NOT "${_actual_angle_patch_sha256}" STREQUAL
       "${_expected_angle_patch_sha256}")
    message(FATAL_ERROR
        "Kernarium iOS ANGLE patch digest does not match its receipt")
endif()

string(LENGTH "${_expected_patch_sha256}" _expected_patch_sha256_length)
if(NOT _expected_patch_sha256_length EQUAL 64)
    message(FATAL_ERROR
        "Kernarium iOS platform receipt lacks the virglrenderer patch digest")
endif()
file(SHA256 "${_installed_virgl_patch}" _actual_patch_sha256)
if(NOT "${_actual_patch_sha256}" STREQUAL "${_expected_patch_sha256}")
    message(FATAL_ERROR
        "Kernarium iOS virglrenderer patch digest does not match its receipt")
endif()

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
