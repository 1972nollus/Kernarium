# SPDX-License-Identifier: GPL-2.0-or-later

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
    "${PREFIX}/share/kernarium/ios-simulator-arm64.tsv")
foreach(_file IN LISTS _required)
    if(NOT EXISTS "${_file}" OR IS_SYMLINK "${_file}")
        message(FATAL_ERROR "Invalid Kernarium iOS GPU dependency: ${_file}")
    endif()
endforeach()

if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required for dependency verification")
endif()
foreach(_receipt IN ITEMS utm-gpu-sources.tsv ios-simulator-arm64.tsv)
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
