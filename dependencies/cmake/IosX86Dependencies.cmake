# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

if(UML_APPLE_PLATFORM STREQUAL "ios")
    set(_uml_ios_x86_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/dependencies/ios-device-arm64/x86")
else()
    set(_uml_ios_x86_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/dependencies/ios-simulator-arm64/x86")
endif()
set(UML_IOS_X86_DEPENDENCY_PREFIX "${_uml_ios_x86_default_prefix}"
    CACHE PATH "Verified SoftFloat/Cephes prefix for the iOS x86 CPU service")

set(_uml_ios_x86_include "${UML_IOS_X86_DEPENDENCY_PREFIX}/include")
set(_uml_ios_x86_lib "${UML_IOS_X86_DEPENDENCY_PREFIX}/lib")
set(_uml_ios_x86_inputs
    "${_uml_ios_x86_lib}/libsoftfloat_3e.a"
    "${_uml_ios_x86_lib}/libcephes_128bit.a"
    "${_uml_ios_x86_include}/SoftFloat-3e/softfloat.h"
    "${_uml_ios_x86_include}/cephes_128bit.h")
foreach(_uml_ios_x86_input IN LISTS _uml_ios_x86_inputs)
    if(NOT EXISTS "${_uml_ios_x86_input}")
        message(FATAL_ERROR
            "Missing iOS x86 CPU-service dependency: ${_uml_ios_x86_input}. "
            "Run dependencies/build-ios-x86.sh first.")
    endif()
endforeach()

add_library(uml_third_party_softfloat STATIC IMPORTED GLOBAL)
set_target_properties(uml_third_party_softfloat PROPERTIES
    IMPORTED_LOCATION "${_uml_ios_x86_lib}/libsoftfloat_3e.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_uml_ios_x86_include}"
    INTERFACE_COMPILE_DEFINITIONS
        "FEXCORE_PRESERVE_ALL_ATTR=;FEXCORE_HAS_PRESERVE_ALL_ATTR=0;SOFTFLOAT_FAST_INT64=1")
add_library(UML::ThirdPartySoftFloat ALIAS uml_third_party_softfloat)

add_library(uml_third_party_cephes_128bit STATIC IMPORTED GLOBAL)
set_target_properties(uml_third_party_cephes_128bit PROPERTIES
    IMPORTED_LOCATION "${_uml_ios_x86_lib}/libcephes_128bit.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_uml_ios_x86_include}"
    INTERFACE_LINK_LIBRARIES UML::ThirdPartySoftFloat)
add_library(UML::ThirdPartyCephes128 ALIAS uml_third_party_cephes_128bit)

add_custom_target(uml_ios_x86_prefix_verify
    COMMAND "${CMAKE_COMMAND}"
        -DPREFIX=${UML_IOS_X86_DEPENDENCY_PREFIX}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -DPLATFORM=${UML_APPLE_PLATFORM}
        -P ${CMAKE_CURRENT_LIST_DIR}/VerifyIosX86Prefix.cmake
    DEPENDS ${_uml_ios_x86_inputs}
    COMMENT "Checking the pinned iOS x86 CPU-service dependency prefix"
    VERBATIM)
if(TARGET uml_dependency_prefix_verify)
    add_dependencies(uml_dependency_prefix_verify uml_ios_x86_prefix_verify)
else()
    add_custom_target(uml_dependency_prefix_verify)
    add_dependencies(uml_dependency_prefix_verify uml_ios_x86_prefix_verify)
endif()
