# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

if(UML_APPLE_PLATFORM STREQUAL "ios")
    set(_uml_ios_network_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/dependencies/ios-device-arm64/network")
else()
    set(_uml_ios_network_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/dependencies/ios-simulator-arm64/network")
endif()
set(UML_IOS_NETWORK_DEPENDENCY_PREFIX
    "${_uml_ios_network_default_prefix}"
    CACHE PATH
    "Verified GLib/libslirp prefix for Kernarium")

set(_uml_ios_network_include "${UML_IOS_NETWORK_DEPENDENCY_PREFIX}/include")
set(_uml_ios_network_lib "${UML_IOS_NETWORK_DEPENDENCY_PREFIX}/lib")
set(_uml_ios_network_inputs
    "${_uml_ios_network_lib}/libpcre2-8.a"
    "${_uml_ios_network_lib}/libffi.a"
    "${_uml_ios_network_lib}/libintl.a"
    "${_uml_ios_network_lib}/libglib-2.0.a"
    "${_uml_ios_network_lib}/libslirp.a"
    "${_uml_ios_network_include}/glib-2.0/glib.h"
    "${_uml_ios_network_include}/slirp/libslirp.h")
foreach(_uml_ios_network_input IN LISTS _uml_ios_network_inputs)
    if(NOT EXISTS "${_uml_ios_network_input}")
        message(FATAL_ERROR
            "Missing Kernarium iOS network dependency: "
            "${_uml_ios_network_input}. Run "
            "dependencies/build-ios-network.sh first.")
    endif()
endforeach()

find_package(Threads REQUIRED)
find_library(_uml_ios_network_math
    NAMES m
    PATHS "${CMAKE_OSX_SYSROOT}/usr/lib"
    NO_DEFAULT_PATH REQUIRED)
find_library(_uml_ios_network_resolver
    NAMES resolv
    PATHS "${CMAKE_OSX_SYSROOT}/usr/lib"
    NO_DEFAULT_PATH REQUIRED)
find_library(_uml_ios_network_iconv
    NAMES iconv
    PATHS "${CMAKE_OSX_SYSROOT}/usr/lib"
    NO_DEFAULT_PATH REQUIRED)
find_library(_uml_ios_network_foundation
    NAMES Foundation
    PATHS "${CMAKE_OSX_SYSROOT}/System/Library/Frameworks"
    NO_DEFAULT_PATH REQUIRED)
find_library(_uml_ios_network_core_foundation
    NAMES CoreFoundation
    PATHS "${CMAKE_OSX_SYSROOT}/System/Library/Frameworks"
    NO_DEFAULT_PATH REQUIRED)

add_library(uml_third_party_pcre2 STATIC IMPORTED GLOBAL)
set_target_properties(uml_third_party_pcre2 PROPERTIES
    IMPORTED_LOCATION "${_uml_ios_network_lib}/libpcre2-8.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_uml_ios_network_include}"
    INTERFACE_COMPILE_DEFINITIONS PCRE2_STATIC
    INTERFACE_LINK_LIBRARIES Threads::Threads)
add_library(UML::ThirdPartyPcre2 ALIAS uml_third_party_pcre2)

add_library(uml_third_party_intl STATIC IMPORTED GLOBAL)
set_target_properties(uml_third_party_intl PROPERTIES
    IMPORTED_LOCATION "${_uml_ios_network_lib}/libintl.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_uml_ios_network_include}"
    INTERFACE_COMPILE_DEFINITIONS G_INTL_STATIC_COMPILATION)
add_library(UML::ThirdPartyIntl ALIAS uml_third_party_intl)

add_library(uml_third_party_glib STATIC IMPORTED GLOBAL)
set_target_properties(uml_third_party_glib PROPERTIES
    IMPORTED_LOCATION "${_uml_ios_network_lib}/libglib-2.0.a"
    INTERFACE_INCLUDE_DIRECTORIES
        "${_uml_ios_network_include}/glib-2.0;${_uml_ios_network_lib}/glib-2.0/include"
    INTERFACE_LINK_LIBRARIES
        "UML::ThirdPartyPcre2;UML::ThirdPartyIntl;Threads::Threads;${_uml_ios_network_iconv};${_uml_ios_network_math};${_uml_ios_network_foundation};${_uml_ios_network_core_foundation}")
add_library(UML::ThirdPartyGlib ALIAS uml_third_party_glib)

add_library(uml_third_party_slirp STATIC IMPORTED GLOBAL)
set_target_properties(uml_third_party_slirp PROPERTIES
    IMPORTED_LOCATION "${_uml_ios_network_lib}/libslirp.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_uml_ios_network_include}/slirp"
    INTERFACE_LINK_LIBRARIES
        "UML::ThirdPartyGlib;${_uml_ios_network_resolver}")
add_library(UML::ThirdPartySlirp ALIAS uml_third_party_slirp)

add_custom_target(uml_ios_network_prefix_verify
    COMMAND "${CMAKE_COMMAND}"
        -DPREFIX=${UML_IOS_NETWORK_DEPENDENCY_PREFIX}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -DPLATFORM=${UML_APPLE_PLATFORM}
        -P ${CMAKE_CURRENT_LIST_DIR}/VerifyIosNetworkPrefix.cmake
    DEPENDS ${_uml_ios_network_inputs}
    COMMENT "Checking the pinned Kernarium iOS network dependency prefix"
    VERBATIM)
