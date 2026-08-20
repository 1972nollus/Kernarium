# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

if(UML_APPLE_PLATFORM STREQUAL "macos")
    set(_uml_apple_gpu_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/dependencies/apple-gpu/macos-arm64")
elseif(UML_APPLE_PLATFORM STREQUAL "ios")
    set(_uml_apple_gpu_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/dependencies/apple-gpu/ios-device-arm64")
elseif(UML_APPLE_PLATFORM STREQUAL "ios-simulator")
    set(_uml_apple_gpu_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/dependencies/apple-gpu/ios-simulator-arm64")
else()
    message(FATAL_ERROR
        "The Apple GPU product does not support ${UML_APPLE_PLATFORM}")
endif()
set(UML_APPLE_GPU_DEPENDENCY_PREFIX
    "${_uml_apple_gpu_default_prefix}"
    CACHE PATH
    "Verified ANGLE/Metal libepoxy/virglrenderer Apple GPU prefix")

set(_uml_apple_gpu_include "${UML_APPLE_GPU_DEPENDENCY_PREFIX}/include")
set(_uml_apple_gpu_lib "${UML_APPLE_GPU_DEPENDENCY_PREFIX}/lib")
set(_uml_apple_gpu_frameworks
    "${UML_APPLE_GPU_DEPENDENCY_PREFIX}/Frameworks")
set(_uml_apple_gpu_inputs
    "${_uml_apple_gpu_lib}/libepoxy.a"
    "${_uml_apple_gpu_lib}/libvirglrenderer.a"
    "${_uml_apple_gpu_include}/epoxy/egl.h"
    "${_uml_apple_gpu_include}/virgl/virglrenderer.h"
    "${_uml_apple_gpu_include}/virgl/virgl_hw.h"
    "${_uml_apple_gpu_frameworks}/EGL.framework/EGL"
    "${_uml_apple_gpu_frameworks}/GLESv2.framework/GLESv2")
foreach(_uml_apple_gpu_input IN LISTS _uml_apple_gpu_inputs)
    if(NOT EXISTS "${_uml_apple_gpu_input}")
        message(FATAL_ERROR
            "Missing ${UML_APPLE_PLATFORM} Apple GPU dependency: "
            "${_uml_apple_gpu_input}. Run dependencies/build-apple-gpu.sh "
            "--platform ${UML_APPLE_PLATFORM} first.")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DPREFIX=${UML_APPLE_GPU_DEPENDENCY_PREFIX}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -DPLATFORM=${UML_APPLE_PLATFORM}
        -P ${CMAKE_CURRENT_LIST_DIR}/VerifyAppleGpuPrefix.cmake
    COMMAND_ERROR_IS_FATAL ANY)

find_package(Threads REQUIRED)
find_library(_uml_apple_gpu_math
    NAMES m
    PATHS "${CMAKE_OSX_SYSROOT}/usr/lib"
    NO_DEFAULT_PATH REQUIRED)

add_library(uml_third_party_angle_egl SHARED IMPORTED GLOBAL)
set_target_properties(uml_third_party_angle_egl PROPERTIES
    IMPORTED_LOCATION "${_uml_apple_gpu_frameworks}/EGL.framework/EGL")
add_library(UML::ThirdPartyAngleEgl ALIAS uml_third_party_angle_egl)

add_library(uml_third_party_angle_gles SHARED IMPORTED GLOBAL)
set_target_properties(uml_third_party_angle_gles PROPERTIES
    IMPORTED_LOCATION
        "${_uml_apple_gpu_frameworks}/GLESv2.framework/GLESv2")
add_library(UML::ThirdPartyAngleGles ALIAS uml_third_party_angle_gles)

add_library(uml_third_party_epoxy STATIC IMPORTED GLOBAL)
set_target_properties(uml_third_party_epoxy PROPERTIES
    IMPORTED_LOCATION "${_uml_apple_gpu_lib}/libepoxy.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_uml_apple_gpu_include}"
    INTERFACE_LINK_LIBRARIES
        "UML::ThirdPartyAngleEgl;UML::ThirdPartyAngleGles")
add_library(UML::ThirdPartyEpoxy ALIAS uml_third_party_epoxy)

add_library(uml_third_party_virglrenderer STATIC IMPORTED GLOBAL)
set_target_properties(uml_third_party_virglrenderer PROPERTIES
    IMPORTED_LOCATION "${_uml_apple_gpu_lib}/libvirglrenderer.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_uml_apple_gpu_include}/virgl"
    INTERFACE_LINK_LIBRARIES
        "UML::ThirdPartyEpoxy;Threads::Threads;${_uml_apple_gpu_math}")
add_library(UML::ThirdPartyVirglrenderer ALIAS
    uml_third_party_virglrenderer)

add_custom_target(uml_apple_gpu_prefix_verify
    COMMAND "${CMAKE_COMMAND}"
        -DPREFIX=${UML_APPLE_GPU_DEPENDENCY_PREFIX}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -DPLATFORM=${UML_APPLE_PLATFORM}
        -P ${CMAKE_CURRENT_LIST_DIR}/VerifyAppleGpuPrefix.cmake
    DEPENDS ${_uml_apple_gpu_inputs}
    COMMENT "Checking the common ${UML_APPLE_PLATFORM} Apple GPU prefix"
    VERBATIM)
if(TARGET uml_dependency_prefix_verify)
    add_dependencies(uml_dependency_prefix_verify
        uml_apple_gpu_prefix_verify)
else()
    add_custom_target(uml_dependency_prefix_verify)
    add_dependencies(uml_dependency_prefix_verify
        uml_apple_gpu_prefix_verify)
endif()

set(UML_APPLE_GPU_FRAMEWORK_DIRECTORY
    "${_uml_apple_gpu_frameworks}"
    CACHE INTERNAL "Verified Apple GPU runtime framework directory")
