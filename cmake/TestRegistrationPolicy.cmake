# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

# Find first-party CMake files which register CTest commands without going
# through UmlTesting.cmake.  CMake command names are case-insensitive, so the
# source text is normalized before matching.
function(uml_find_raw_test_registrations)
    cmake_parse_arguments(ARG "" "SOURCE_ROOT;OUT_VIOLATIONS" "" ${ARGN})
    if(NOT ARG_SOURCE_ROOT OR NOT ARG_OUT_VIOLATIONS)
        message(FATAL_ERROR
            "uml_find_raw_test_registrations requires SOURCE_ROOT and OUT_VIOLATIONS")
    endif()

    set(_manifests "${ARG_SOURCE_ROOT}/CMakeLists.txt")
    foreach(_scope IN ITEMS android cmake darwin dependencies kernel Kernarium tests)
        if(NOT IS_DIRECTORY "${ARG_SOURCE_ROOT}/${_scope}")
            continue()
        endif()
        file(GLOB_RECURSE _scoped_manifests LIST_DIRECTORIES false
            "${ARG_SOURCE_ROOT}/${_scope}/CMakeLists.txt"
            "${ARG_SOURCE_ROOT}/${_scope}/*.cmake")
        list(APPEND _manifests ${_scoped_manifests})
    endforeach()
    list(REMOVE_DUPLICATES _manifests)

    set(_violations)
    foreach(_path IN LISTS _manifests)
        if(_path STREQUAL "${ARG_SOURCE_ROOT}/cmake/UmlTesting.cmake")
            continue()
        endif()
        file(READ "${_path}" _code)
        string(REGEX REPLACE "#[^\n]*" "" _code "${_code}")
        string(TOLOWER "${_code}" _normalized_code)
        if(_normalized_code MATCHES
           "(^|[\r\n])[ \t]*add_test[ \t\r\n]*\\(")
            list(APPEND _violations
                "${_path}: raw add_test bypasses the centralized suite helpers")
        endif()
    endforeach()
    set(${ARG_OUT_VIOLATIONS} "${_violations}" PARENT_SCOPE)
endfunction()
