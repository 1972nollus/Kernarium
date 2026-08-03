# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

function(_uml_import_static_dependency name archive)
    set(options)
    set(one_value)
    set(multi_value INCLUDES DEFINITIONS LINKS)
    cmake_parse_arguments(ARG "${options}" "${one_value}" "${multi_value}"
        ${ARGN})

    set(_target "uml_third_party_${name}")
    add_library(${_target} STATIC IMPORTED GLOBAL)
    set_target_properties(${_target} PROPERTIES
        IMPORTED_LOCATION "${archive}")
    if(ARG_INCLUDES)
        set_target_properties(${_target} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${ARG_INCLUDES}")
    endif()
    if(ARG_DEFINITIONS)
        set_target_properties(${_target} PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS "${ARG_DEFINITIONS}")
    endif()
    if(ARG_LINKS)
        set_target_properties(${_target} PROPERTIES
            INTERFACE_LINK_LIBRARIES "${ARG_LINKS}")
    endif()
endfunction()

function(_uml_find_sdk_library result name)
    find_library(${result}
        NAMES "${name}"
        PATHS "${CMAKE_OSX_SYSROOT}/usr/lib"
        NO_DEFAULT_PATH REQUIRED)
    set(${result} "${${result}}" PARENT_SCOPE)
endfunction()

function(_uml_find_sdk_framework result name)
    find_library(${result}
        NAMES "${name}"
        PATHS "${CMAKE_OSX_SYSROOT}/System/Library/Frameworks"
        NO_DEFAULT_PATH REQUIRED)
    set(${result} "${${result}}" PARENT_SCOPE)
endfunction()

function(uml_define_third_party_targets prefix)
    _uml_find_sdk_library(_uml_sdk_math m)
    _uml_find_sdk_library(_uml_sdk_resolver resolv)
    foreach(_framework IN ITEMS Foundation CoreFoundation AppKit Carbon)
        _uml_find_sdk_framework(_uml_sdk_${_framework} ${_framework})
    endforeach()

    set(_include "${prefix}/include")
    set(_lib "${prefix}/lib")

    _uml_import_static_dependency(zlib "${_lib}/libz.a"
        INCLUDES "${_include}")
    add_library(UML::ThirdPartyZlib ALIAS uml_third_party_zlib)

    _uml_import_static_dependency(pcre2 "${_lib}/libpcre2-8.a"
        INCLUDES "${_include}"
        DEFINITIONS PCRE2_STATIC
        LINKS Threads::Threads)
    add_library(UML::ThirdPartyPcre2 ALIAS uml_third_party_pcre2)

    _uml_import_static_dependency(charset "${_lib}/libcharset.a"
        INCLUDES "${_include}")
    add_library(UML::ThirdPartyCharset ALIAS uml_third_party_charset)

    _uml_import_static_dependency(iconv "${_lib}/libiconv.a"
        INCLUDES "${_include}"
        LINKS UML::ThirdPartyCharset)
    add_library(UML::ThirdPartyIconv ALIAS uml_third_party_iconv)

    _uml_import_static_dependency(intl "${_lib}/libintl.a"
        INCLUDES "${_include}"
        DEFINITIONS G_INTL_STATIC_COMPILATION)
    add_library(UML::ThirdPartyIntl ALIAS uml_third_party_intl)

    _uml_import_static_dependency(glib "${_lib}/libglib-2.0.a"
        INCLUDES
            "${_include}/glib-2.0"
            "${_lib}/glib-2.0/include"
        LINKS
            UML::ThirdPartyPcre2
            UML::ThirdPartyIntl
            UML::ThirdPartyIconv
            Threads::Threads
            "${_uml_sdk_math}"
            "${_uml_sdk_Foundation}"
            "${_uml_sdk_CoreFoundation}"
            "${_uml_sdk_AppKit}"
            "${_uml_sdk_Carbon}")
    add_library(UML::ThirdPartyGlib ALIAS uml_third_party_glib)

    _uml_import_static_dependency(slirp "${_lib}/libslirp.a"
        INCLUDES "${_include}/slirp"
        LINKS UML::ThirdPartyGlib "${_uml_sdk_resolver}")
    add_library(UML::ThirdPartySlirp ALIAS uml_third_party_slirp)

    _uml_import_static_dependency(epoxy "${_lib}/libepoxy.a"
        INCLUDES "${_include}")
    add_library(UML::ThirdPartyEpoxy ALIAS uml_third_party_epoxy)

    _uml_import_static_dependency(virglrenderer
        "${_lib}/libvirglrenderer.a"
        INCLUDES "${_include}/virgl"
        LINKS UML::ThirdPartyEpoxy Threads::Threads "${_uml_sdk_math}")
    add_library(UML::ThirdPartyVirglrenderer ALIAS
        uml_third_party_virglrenderer)

    _uml_import_static_dependency(vnc_server "${_lib}/libvncserver.a"
        INCLUDES "${_include}"
        LINKS UML::ThirdPartyZlib Threads::Threads)
    add_library(UML::ThirdPartyVncServer ALIAS uml_third_party_vnc_server)

    _uml_import_static_dependency(vnc_client "${_lib}/libvncclient.a"
        INCLUDES "${_include}"
        LINKS UML::ThirdPartyZlib Threads::Threads)
    add_library(UML::ThirdPartyVncClient ALIAS uml_third_party_vnc_client)
endfunction()
