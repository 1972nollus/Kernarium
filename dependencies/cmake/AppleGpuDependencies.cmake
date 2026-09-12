# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

set(UML_APPLE_GPU_PRODUCT "apple-gpu" CACHE STRING
    "Verified Apple GPU dependency product")
set_property(CACHE UML_APPLE_GPU_PRODUCT PROPERTY STRINGS
    apple-gpu apple-gpu-vulkan-experiment)
if(UML_APPLE_GPU_PRODUCT STREQUAL "apple-gpu")
    set(_uml_apple_gpu_vulkan FALSE)
    set(_uml_apple_gl_provider angle-metal)
elseif(UML_APPLE_GPU_PRODUCT STREQUAL "apple-gpu-vulkan-experiment")
    set(_uml_apple_gpu_vulkan TRUE)
    set(_uml_apple_gl_provider zink-kosmickrisp)
    if(NOT UML_APPLE_PLATFORM STREQUAL "macos")
        message(FATAL_ERROR
            "The KosmicKrisp Vulkan product currently supports macOS only")
    endif()
else()
    message(FATAL_ERROR
        "Unsupported Apple GPU dependency product: ${UML_APPLE_GPU_PRODUCT}")
endif()

if(UML_APPLE_PLATFORM STREQUAL "macos")
    set(_uml_apple_gpu_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/dependencies/${UML_APPLE_GPU_PRODUCT}/macos-arm64")
elseif(UML_APPLE_PLATFORM STREQUAL "ios")
    set(_uml_apple_gpu_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/dependencies/${UML_APPLE_GPU_PRODUCT}/ios-device-arm64")
elseif(UML_APPLE_PLATFORM STREQUAL "ios-simulator")
    set(_uml_apple_gpu_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/dependencies/${UML_APPLE_GPU_PRODUCT}/ios-simulator-arm64")
else()
    message(FATAL_ERROR
        "The Apple GPU product does not support ${UML_APPLE_PLATFORM}")
endif()
set(UML_APPLE_GPU_DEPENDENCY_PREFIX
    "${_uml_apple_gpu_default_prefix}"
    CACHE PATH
    "Verified Apple GPU prefix: host GL provider, libepoxy, virglrenderer")

set(_uml_apple_gpu_include "${UML_APPLE_GPU_DEPENDENCY_PREFIX}/include")
set(_uml_apple_gpu_lib "${UML_APPLE_GPU_DEPENDENCY_PREFIX}/lib")
set(_uml_apple_gpu_frameworks
    "${UML_APPLE_GPU_DEPENDENCY_PREFIX}/Frameworks")
set(_uml_apple_gpu_inputs
    "${_uml_apple_gpu_lib}/libepoxy.a"
    "${_uml_apple_gpu_lib}/libvirglrenderer.a"
    "${_uml_apple_gpu_include}/epoxy/egl.h"
    "${_uml_apple_gpu_include}/virgl/virglrenderer.h"
    "${_uml_apple_gpu_include}/virgl/virgl_hw.h")
if(_uml_apple_gl_provider STREQUAL "angle-metal")
    set(_uml_apple_gl_frameworks EGL GLESv2)
    set(_uml_apple_gl_provider_is_mesa FALSE)
else()
    set(_uml_apple_gl_frameworks MesaEGL MesaGallium)
    set(_uml_apple_gl_provider_is_mesa TRUE)
endif()
foreach(_uml_apple_gl_framework IN LISTS _uml_apple_gl_frameworks)
    list(APPEND _uml_apple_gpu_inputs
        "${_uml_apple_gpu_frameworks}/${_uml_apple_gl_framework}.framework/${_uml_apple_gl_framework}")
endforeach()
if(_uml_apple_gpu_vulkan)
    list(APPEND _uml_apple_gpu_inputs
        "${_uml_apple_gpu_include}/vulkan/vulkan.h"
        "${_uml_apple_gpu_include}/vulkan/vulkan_core.h"
        "${PROJECT_SOURCE_DIR}/tools/rootfs/vulkan/patches/venus-protocol/series.tsv"
        "${_uml_apple_gpu_frameworks}/KosmicKrisp.framework/KosmicKrisp"
        "${_uml_apple_gpu_frameworks}/KosmicKrisp.framework/Modules/module.modulemap")
endif()
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
        -DPRODUCT=${UML_APPLE_GPU_PRODUCT}
        -P ${CMAKE_CURRENT_LIST_DIR}/VerifyAppleGpuPrefix.cmake
    COMMAND_ERROR_IS_FATAL ANY)

find_package(Threads REQUIRED)
find_library(_uml_apple_gpu_math
    NAMES m
    PATHS "${CMAKE_OSX_SYSROOT}/usr/lib"
    NO_DEFAULT_PATH REQUIRED)

# The packaged host GL provider is one product decision: ANGLE's EGL/GLES
# facade, or Mesa's EGL entry point over its Gallium library. libepoxy and the
# renderer dispatch through whichever pair the prefix carries.
set(_uml_apple_gl_provider_targets)
foreach(_uml_apple_gl_framework IN LISTS _uml_apple_gl_frameworks)
    string(TOLOWER "${_uml_apple_gl_framework}" _uml_apple_gl_lower)
    set(_uml_apple_gl_target "uml_third_party_host_gl_${_uml_apple_gl_lower}")
    add_library(${_uml_apple_gl_target} SHARED IMPORTED GLOBAL)
    set_target_properties(${_uml_apple_gl_target} PROPERTIES
        IMPORTED_LOCATION
            "${_uml_apple_gpu_frameworks}/${_uml_apple_gl_framework}.framework/${_uml_apple_gl_framework}")
    list(APPEND _uml_apple_gl_provider_targets "${_uml_apple_gl_target}")
endforeach()
list(GET _uml_apple_gl_provider_targets 0 _uml_apple_gl_egl_target)
add_library(UML::ThirdPartyHostGlEgl ALIAS ${_uml_apple_gl_egl_target})

add_library(uml_third_party_epoxy STATIC IMPORTED GLOBAL)
set_target_properties(uml_third_party_epoxy PROPERTIES
    IMPORTED_LOCATION "${_uml_apple_gpu_lib}/libepoxy.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_uml_apple_gpu_include}"
    INTERFACE_LINK_LIBRARIES
        "${_uml_apple_gl_provider_targets}")
add_library(UML::ThirdPartyEpoxy ALIAS uml_third_party_epoxy)

if(_uml_apple_gpu_vulkan)
    add_library(uml_third_party_kosmickrisp SHARED IMPORTED GLOBAL)
    set_target_properties(uml_third_party_kosmickrisp PROPERTIES
        IMPORTED_LOCATION
            "${_uml_apple_gpu_frameworks}/KosmicKrisp.framework/KosmicKrisp"
        INTERFACE_INCLUDE_DIRECTORIES "${_uml_apple_gpu_include}")
    add_library(UML::ThirdPartyKosmicKrisp ALIAS
        uml_third_party_kosmickrisp)
endif()

set(_uml_apple_gpu_virgl_links
    UML::ThirdPartyEpoxy Threads::Threads "${_uml_apple_gpu_math}")
if(_uml_apple_gpu_vulkan)
    list(APPEND _uml_apple_gpu_virgl_links UML::ThirdPartyKosmicKrisp)
endif()
add_library(uml_third_party_virglrenderer STATIC IMPORTED GLOBAL)
set_target_properties(uml_third_party_virglrenderer PROPERTIES
    IMPORTED_LOCATION "${_uml_apple_gpu_lib}/libvirglrenderer.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_uml_apple_gpu_include}/virgl"
    INTERFACE_LINK_LIBRARIES "${_uml_apple_gpu_virgl_links}")
add_library(UML::ThirdPartyVirglrenderer ALIAS
    uml_third_party_virglrenderer)

add_custom_target(uml_apple_gpu_prefix_verify
    COMMAND "${CMAKE_COMMAND}"
        -DPREFIX=${UML_APPLE_GPU_DEPENDENCY_PREFIX}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -DPLATFORM=${UML_APPLE_PLATFORM}
        -DPRODUCT=${UML_APPLE_GPU_PRODUCT}
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
set(UML_APPLE_GPU_GL_FRAMEWORK_NAMES
    "${_uml_apple_gl_frameworks}"
    CACHE INTERNAL "Packaged Apple host GL provider frameworks")
if(_uml_apple_gpu_vulkan)
    set(UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK
        "${_uml_apple_gpu_frameworks}/KosmicKrisp.framework"
        CACHE INTERNAL "Verified KosmicKrisp runtime framework")
endif()
