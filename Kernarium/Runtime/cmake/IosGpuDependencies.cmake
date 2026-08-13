# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

if(UML_APPLE_PLATFORM STREQUAL "ios")
    set(_uml_ios_gpu_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/kernarium-gpu-dependencies/ios-device-arm64")
else()
    set(_uml_ios_gpu_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/kernarium-gpu-dependencies/ios-simulator-arm64")
endif()
set(UML_IOS_GPU_DEPENDENCY_PREFIX
    "${_uml_ios_gpu_default_prefix}"
    CACHE PATH
    "Verified UTM ANGLE/libepoxy/virglrenderer prefix for Kernarium")

set(_uml_ios_gpu_include "${UML_IOS_GPU_DEPENDENCY_PREFIX}/include")
set(_uml_ios_gpu_lib "${UML_IOS_GPU_DEPENDENCY_PREFIX}/lib")
set(_uml_ios_gpu_frameworks
    "${UML_IOS_GPU_DEPENDENCY_PREFIX}/Frameworks")
set(_uml_ios_gpu_inputs
    "${_uml_ios_gpu_lib}/libepoxy.a"
    "${_uml_ios_gpu_lib}/libvirglrenderer.a"
    "${_uml_ios_gpu_include}/epoxy/egl.h"
    "${_uml_ios_gpu_include}/virgl/virglrenderer.h"
    "${_uml_ios_gpu_include}/virgl/virgl_hw.h"
    "${_uml_ios_gpu_frameworks}/EGL.framework/EGL"
    "${_uml_ios_gpu_frameworks}/GLESv2.framework/GLESv2")
foreach(_uml_ios_gpu_input IN LISTS _uml_ios_gpu_inputs)
    if(NOT EXISTS "${_uml_ios_gpu_input}")
        message(FATAL_ERROR
            "Missing Kernarium iOS GPU dependency: ${_uml_ios_gpu_input}. "
            "Run Kernarium/scripts/build-ios-gpu-dependencies.sh first.")
    endif()
endforeach()

find_package(Threads REQUIRED)
find_library(_uml_ios_gpu_math
    NAMES m
    PATHS "${CMAKE_OSX_SYSROOT}/usr/lib"
    NO_DEFAULT_PATH REQUIRED)

add_library(uml_third_party_epoxy STATIC IMPORTED GLOBAL)
set_target_properties(uml_third_party_epoxy PROPERTIES
    IMPORTED_LOCATION "${_uml_ios_gpu_lib}/libepoxy.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_uml_ios_gpu_include}")
add_library(UML::ThirdPartyEpoxy ALIAS uml_third_party_epoxy)

add_library(uml_third_party_virglrenderer STATIC IMPORTED GLOBAL)
set_target_properties(uml_third_party_virglrenderer PROPERTIES
    IMPORTED_LOCATION "${_uml_ios_gpu_lib}/libvirglrenderer.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_uml_ios_gpu_include}/virgl"
    INTERFACE_LINK_LIBRARIES
        "UML::ThirdPartyEpoxy;Threads::Threads;${_uml_ios_gpu_math}")
add_library(UML::ThirdPartyVirglrenderer ALIAS
    uml_third_party_virglrenderer)

add_custom_target(uml_dependency_prefix_verify
    COMMAND "${CMAKE_COMMAND}"
        -DPREFIX=${UML_IOS_GPU_DEPENDENCY_PREFIX}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -DPLATFORM=${UML_APPLE_PLATFORM}
        -P ${CMAKE_CURRENT_LIST_DIR}/VerifyIosGpuPrefix.cmake
    DEPENDS ${_uml_ios_gpu_inputs}
    COMMENT "Checking the pinned Kernarium iOS GPU dependency prefix"
    VERBATIM)
