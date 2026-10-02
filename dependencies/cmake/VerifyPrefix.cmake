# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

# A patched source is identified by the tree its generated series records.
# Builders install source-trees.tsv from the prepared sources; the prefix is
# current exactly while every listed tree matches its series.
#
# uml_verify_source_trees(INSTALLED <file> LABEL <text>
#     SOURCES <name> <series.tsv> [<name> <series.tsv> ...])
function(uml_verify_source_trees)
    cmake_parse_arguments(PARSE_ARGV 0 _arg "" "INSTALLED;LABEL" "SOURCES")
    if(NOT EXISTS "${_arg_INSTALLED}" OR IS_SYMLINK "${_arg_INSTALLED}")
        message(FATAL_ERROR
            "${_arg_LABEL} source trees are missing: ${_arg_INSTALLED}")
    endif()
    file(STRINGS "${_arg_INSTALLED}" _rows)
    set(_installed_names)
    foreach(_row IN LISTS _rows)
        if(_row STREQUAL "source\ttree")
            continue()
        endif()
        if(NOT _row MATCHES "^([a-z0-9-]+)\t([0-9a-f]+)$")
            message(FATAL_ERROR
                "Malformed ${_arg_LABEL} source-tree row: ${_row}")
        endif()
        list(FIND _installed_names "${CMAKE_MATCH_1}" _seen)
        if(NOT _seen EQUAL -1)
            message(FATAL_ERROR
                "${_arg_LABEL} source tree is listed twice: ${CMAKE_MATCH_1}")
        endif()
        list(APPEND _installed_names "${CMAKE_MATCH_1}")
        set(_installed_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endforeach()

    set(_expected_names)
    set(_pairs ${_arg_SOURCES})
    while(_pairs)
        list(POP_FRONT _pairs _name _series)
        list(APPEND _expected_names "${_name}")
        if(NOT EXISTS "${_series}" OR IS_SYMLINK "${_series}")
            message(FATAL_ERROR "Patch series is missing: ${_series}")
        endif()
        file(STRINGS "${_series}" _series_trees REGEX "^tree\t")
        list(LENGTH _series_trees _tree_count)
        if(NOT _tree_count EQUAL 1 OR
                NOT _series_trees MATCHES "^tree\t([0-9a-f]+)$")
            message(FATAL_ERROR "Patch series records no tree: ${_series}")
        endif()
        if(NOT "${_installed_${_name}}" STREQUAL "${CMAKE_MATCH_1}")
            message(FATAL_ERROR
                "${_arg_LABEL} ${_name} was built from tree "
                "'${_installed_${_name}}'; its series records ${CMAKE_MATCH_1}. "
                "Rebuild the prefix from sources prepared by the current series.")
        endif()
    endwhile()
    list(SORT _installed_names)
    list(SORT _expected_names)
    if(NOT _installed_names STREQUAL _expected_names)
        message(FATAL_ERROR
            "${_arg_LABEL} source trees list ${_installed_names}, "
            "expected ${_expected_names}")
    endif()
endfunction()
