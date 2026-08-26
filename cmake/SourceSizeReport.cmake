# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

find_program(_UML_SOURCE_SIZE_WC_EXECUTABLE NAMES wc NO_CACHE)

function(_uml_source_size_count_lines OUTPUT_VARIABLE PATH)
    file(SIZE "${PATH}" _uml_source_size_length)
    if(_uml_source_size_length EQUAL 0)
        set(_uml_source_size_line_count 0)
    elseif(_UML_SOURCE_SIZE_WC_EXECUTABLE)
        # Avoid copying each complete file through CMake's regex engine. Some
        # checked-in declarative inventories are tens of megabytes, while the
        # source-size report needs only a newline count and the final byte.
        execute_process(
            COMMAND "${_UML_SOURCE_SIZE_WC_EXECUTABLE}" -l "${PATH}"
            RESULT_VARIABLE _uml_source_size_wc_result
            OUTPUT_VARIABLE _uml_source_size_wc_output
            ERROR_VARIABLE _uml_source_size_wc_error)
        if(NOT _uml_source_size_wc_result EQUAL 0 OR
           NOT _uml_source_size_wc_output MATCHES
               "^[ \t]*([0-9]+)[ \t]+")
            message(FATAL_ERROR
                "Unable to count source lines in ${PATH}: "
                "${_uml_source_size_wc_error}")
        endif()
        set(_uml_source_size_line_count "${CMAKE_MATCH_1}")
        math(EXPR _uml_source_size_last_index
            "${_uml_source_size_length} - 1")
        file(READ "${PATH}" _uml_source_size_last_character
            OFFSET ${_uml_source_size_last_index} LIMIT 1)
        if(NOT _uml_source_size_last_character STREQUAL "\n")
            math(EXPR _uml_source_size_line_count
                "${_uml_source_size_line_count} + 1")
        endif()
    else()
        # CMake-only fallback for non-POSIX hosts. The supported Apple build
        # environment always supplies wc as a host build tool.
        file(READ "${PATH}" _uml_source_size_contents)
        string(REGEX REPLACE "[^\n]" "" _uml_source_size_newlines
            "${_uml_source_size_contents}")
        string(LENGTH "${_uml_source_size_newlines}"
            _uml_source_size_line_count)
        math(EXPR _uml_source_size_last_index
            "${_uml_source_size_length} - 1")
        string(SUBSTRING "${_uml_source_size_contents}"
            ${_uml_source_size_last_index} 1 _uml_source_size_last_character)
        if(NOT _uml_source_size_last_character STREQUAL "\n")
            math(EXPR _uml_source_size_line_count
                "${_uml_source_size_line_count} + 1")
        endif()
    endif()
    set(${OUTPUT_VARIABLE} "${_uml_source_size_line_count}" PARENT_SCOPE)
endfunction()

function(uml_report_source_sizes)
    set(_uml_source_size_one_value_arguments
        SOURCE_ROOT
        REPORT_PATH)
    cmake_parse_arguments(UML_SOURCE_SIZE ""
        "${_uml_source_size_one_value_arguments}" "" ${ARGN})

    if(NOT UML_SOURCE_SIZE_SOURCE_ROOT)
        message(FATAL_ERROR
            "uml_report_source_sizes requires a SOURCE_ROOT")
    endif()

    cmake_path(ABSOLUTE_PATH UML_SOURCE_SIZE_SOURCE_ROOT
        NORMALIZE OUTPUT_VARIABLE _uml_source_size_root)
    if(NOT IS_DIRECTORY "${_uml_source_size_root}")
        message(FATAL_ERROR
            "Source-size SOURCE_ROOT is not a directory: ${_uml_source_size_root}")
    endif()

    set(_uml_source_size_all_files)
    if(EXISTS "${_uml_source_size_root}/CMakeLists.txt")
        list(APPEND _uml_source_size_all_files
            "${_uml_source_size_root}/CMakeLists.txt")
    endif()
    foreach(_uml_source_size_scope IN ITEMS
            cmake
            darwin
            dependencies
            kernel
            scripts
            tests
            tools)
        if(IS_DIRECTORY
                "${_uml_source_size_root}/${_uml_source_size_scope}")
            file(GLOB_RECURSE _uml_source_size_scope_files
                LIST_DIRECTORIES FALSE
                "${_uml_source_size_root}/${_uml_source_size_scope}/*")
            list(APPEND _uml_source_size_all_files
                ${_uml_source_size_scope_files})
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _uml_source_size_all_files)

    set(_uml_source_size_code_extensions
        .c .cc .cpp .cxx
        .m .mm
        .h .hh .hpp .hxx
        .ipp .inl .def
        .s .asm .inc
        .py .sh .bash .zsh .mk)
    set(_uml_source_size_declarative_extensions
        .asc .asl .commands .env .fragment
        .json .md .patch .toml .tsv .txt .yaml .yml)
    set(_uml_source_size_paths)
    set(_uml_source_size_declarative_paths)
    foreach(_uml_source_size_path IN LISTS _uml_source_size_all_files)
        file(RELATIVE_PATH _uml_source_size_relative_path
            "${_uml_source_size_root}" "${_uml_source_size_path}")
        cmake_path(CONVERT "${_uml_source_size_relative_path}"
            TO_CMAKE_PATH_LIST _uml_source_size_relative_path NORMALIZE)
        get_filename_component(_uml_source_size_name
            "${_uml_source_size_relative_path}" NAME)
        get_filename_component(_uml_source_size_extension
            "${_uml_source_size_relative_path}" EXT)
        string(TOLOWER "${_uml_source_size_extension}"
            _uml_source_size_extension)
        list(FIND _uml_source_size_code_extensions
            "${_uml_source_size_extension}" _uml_source_size_code_index)
        list(FIND _uml_source_size_declarative_extensions
            "${_uml_source_size_extension}"
            _uml_source_size_declarative_index)

        set(_uml_source_size_is_reported FALSE)
        set(_uml_source_size_is_declarative FALSE)
        if(_uml_source_size_name STREQUAL "CMakeLists.txt" OR
           _uml_source_size_extension STREQUAL ".cmake")
            set(_uml_source_size_is_reported TRUE)
        elseif(NOT _uml_source_size_code_index EQUAL -1)
            set(_uml_source_size_is_reported TRUE)
        elseif(NOT _uml_source_size_declarative_index EQUAL -1)
            set(_uml_source_size_is_reported TRUE)
            set(_uml_source_size_is_declarative TRUE)
        else()
            # Rootfs helpers and toolchain wrappers may omit an extension or
            # use a command-like suffix such as mkfs.ext2. Report scripts by
            # their shebang and keep extensionless rootfs configuration data
            # visible where its role is known.
            file(STRINGS "${_uml_source_size_path}"
                _uml_source_size_first_line LIMIT_COUNT 1)
            if(_uml_source_size_first_line MATCHES "^#!" OR
               _uml_source_size_name STREQUAL "Makefile")
                set(_uml_source_size_is_reported TRUE)
            elseif(_uml_source_size_extension STREQUAL "" AND
                   _uml_source_size_relative_path MATCHES
                    "^darwin/rootfs/(inittab|profile)$")
                set(_uml_source_size_is_reported TRUE)
                if(_uml_source_size_name STREQUAL "inittab")
                    set(_uml_source_size_is_declarative TRUE)
                endif()
            endif()
        endif()

        if(_uml_source_size_is_reported)
            list(APPEND _uml_source_size_paths
                "${_uml_source_size_relative_path}")
            if(_uml_source_size_is_declarative)
                list(APPEND _uml_source_size_declarative_paths
                    "${_uml_source_size_relative_path}")
            endif()
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _uml_source_size_paths)
    list(SORT _uml_source_size_paths)
    list(REMOVE_DUPLICATES _uml_source_size_declarative_paths)
    list(SORT _uml_source_size_declarative_paths)

    set(_uml_source_size_report
        "category\tguideline\tactual_lines\tover_by\tpath\tstatus\n")
    set(_uml_source_size_total 0)
    set(_uml_source_size_above 0)
    foreach(_uml_source_size_relative_path IN LISTS _uml_source_size_paths)
        get_filename_component(_uml_source_size_name
            "${_uml_source_size_relative_path}" NAME)
        get_filename_component(_uml_source_size_extension
            "${_uml_source_size_relative_path}" EXT)
        string(TOLOWER "${_uml_source_size_extension}"
            _uml_source_size_extension)
        list(FIND _uml_source_size_declarative_paths
            "${_uml_source_size_relative_path}"
            _uml_source_size_declarative_index)

        if(_uml_source_size_name STREQUAL "CMakeLists.txt" OR
           _uml_source_size_extension STREQUAL ".cmake")
            set(_uml_source_size_category cmake)
            set(_uml_source_size_guideline 300)
        elseif(NOT _uml_source_size_declarative_index EQUAL -1)
            set(_uml_source_size_category declarative)
            set(_uml_source_size_guideline -)
        elseif(_uml_source_size_relative_path MATCHES "^tests/")
            set(_uml_source_size_category test)
            set(_uml_source_size_guideline 600)
        elseif(_uml_source_size_relative_path MATCHES
                "^(cmake|dependencies|kernel|scripts|tools)/" OR
               _uml_source_size_relative_path MATCHES "^darwin/rootfs/")
            set(_uml_source_size_category support)
            set(_uml_source_size_guideline 600)
        else()
            set(_uml_source_size_category production)
            set(_uml_source_size_guideline 800)
        endif()

        _uml_source_size_count_lines(_uml_source_size_actual_lines
            "${_uml_source_size_root}/${_uml_source_size_relative_path}")
        if(_uml_source_size_guideline STREQUAL "-")
            set(_uml_source_size_over_by -)
            set(_uml_source_size_is_above FALSE)
        elseif(_uml_source_size_actual_lines GREATER
                _uml_source_size_guideline)
            math(EXPR _uml_source_size_over_by
                "${_uml_source_size_actual_lines} - ${_uml_source_size_guideline}")
            set(_uml_source_size_is_above TRUE)
            math(EXPR _uml_source_size_above
                "${_uml_source_size_above} + 1")
        else()
            set(_uml_source_size_over_by 0)
            set(_uml_source_size_is_above FALSE)
        endif()

        if(_uml_source_size_category STREQUAL "declarative")
            set(_uml_source_size_status informational)
        elseif(_uml_source_size_is_above)
            set(_uml_source_size_status above-guideline)
        else()
            set(_uml_source_size_status within-guideline)
        endif()

        math(EXPR _uml_source_size_total "${_uml_source_size_total} + 1")
        string(APPEND _uml_source_size_report
            "${_uml_source_size_category}\t${_uml_source_size_guideline}\t${_uml_source_size_actual_lines}\t${_uml_source_size_over_by}\t${_uml_source_size_relative_path}\t${_uml_source_size_status}\n")

        if(NOT UML_SOURCE_SIZE_REPORT_PATH AND _uml_source_size_is_above)
            message(STATUS
                "Source-size advisory: ${_uml_source_size_relative_path}: ${_uml_source_size_actual_lines} lines, ${_uml_source_size_status} (guideline ${_uml_source_size_guideline})")
        endif()
    endforeach()

    if(UML_SOURCE_SIZE_REPORT_PATH)
        get_filename_component(_uml_source_size_report_directory
            "${UML_SOURCE_SIZE_REPORT_PATH}" DIRECTORY)
        if(_uml_source_size_report_directory)
            file(MAKE_DIRECTORY "${_uml_source_size_report_directory}")
        endif()
        file(WRITE "${UML_SOURCE_SIZE_REPORT_PATH}"
            "${_uml_source_size_report}")
        message(STATUS
            "Source-size advisory report: ${UML_SOURCE_SIZE_REPORT_PATH}")
    endif()
    message(STATUS
        "Source-size advisory summary: ${_uml_source_size_total} files, ${_uml_source_size_above} above guideline")
endfunction()
