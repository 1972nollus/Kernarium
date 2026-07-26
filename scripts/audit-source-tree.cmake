# SPDX-License-Identifier: GPL-2.0-or-later
if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(GLOB_RECURSE C_SOURCES LIST_DIRECTORIES false "${SOURCE_ROOT}/*.c")
file(GLOB_RECURSE RAW_MAKEFILES LIST_DIRECTORIES false
    "${SOURCE_ROOT}/Makefile")

set(VIOLATIONS)
foreach(PATH IN LISTS C_SOURCES RAW_MAKEFILES)
    if(PATH MATCHES "/linux-6\\.18/" OR
       PATH MATCHES "/external/" OR
       PATH MATCHES "/\\.build/" OR
       PATH MATCHES "/\\.git/")
        continue()
    endif()
    list(APPEND VIOLATIONS "${PATH}")
endforeach()

if(VIOLATIONS)
    list(JOIN VIOLATIONS "\n  " FORMATTED_VIOLATIONS)
    message(FATAL_ERROR
        "Host source must use C++/assembly and CMake only:\n  ${FORMATTED_VIOLATIONS}")
endif()
