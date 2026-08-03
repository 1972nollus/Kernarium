# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

function(_uml_source_size_count_lines OUTPUT_VARIABLE PATH)
    file(READ "${PATH}" _uml_source_size_contents)
    string(LENGTH "${_uml_source_size_contents}" _uml_source_size_length)
    if(_uml_source_size_length EQUAL 0)
        set(_uml_source_size_line_count 0)
    else()
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
        REVIEW_MANIFEST
        REPORT_PATH
        OUT_ERRORS)
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

    if(UML_SOURCE_SIZE_REVIEW_MANIFEST)
        set(_uml_source_size_manifest
            "${UML_SOURCE_SIZE_REVIEW_MANIFEST}")
    else()
        set(_uml_source_size_manifest
            "${_uml_source_size_root}/docs/source-size-reviews.tsv")
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
        .asc .commands .env .fragment
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

    set(_uml_source_size_errors)
    set(_uml_source_size_review_paths)
    set(_uml_source_size_review_owners)
    set(_uml_source_size_review_lines)
    set(_uml_source_size_review_notes)
    set(_uml_source_size_review_dates)
    if(EXISTS "${_uml_source_size_manifest}")
        file(STRINGS "${_uml_source_size_manifest}"
            _uml_source_size_manifest_rows)
        set(_uml_source_size_header_seen FALSE)
        foreach(_uml_source_size_row IN LISTS _uml_source_size_manifest_rows)
            if(_uml_source_size_row STREQUAL "" OR
               _uml_source_size_row MATCHES "^#")
                continue()
            endif()
            if(NOT _uml_source_size_header_seen)
                set(_uml_source_size_expected_header
                    "path\towner\treviewed_lines\tnote\treviewed")
                if(NOT _uml_source_size_row STREQUAL
                        "${_uml_source_size_expected_header}")
                    list(APPEND _uml_source_size_errors
                        "${_uml_source_size_manifest}: expected header '${_uml_source_size_expected_header}'")
                endif()
                set(_uml_source_size_header_seen TRUE)
                continue()
            endif()

            string(REPLACE "\t" ";" _uml_source_size_fields
                "${_uml_source_size_row}")
            list(LENGTH _uml_source_size_fields
                _uml_source_size_field_count)
            if(NOT _uml_source_size_field_count EQUAL 5)
                list(APPEND _uml_source_size_errors
                    "${_uml_source_size_manifest}: malformed row '${_uml_source_size_row}'")
                continue()
            endif()

            list(GET _uml_source_size_fields 0 _uml_source_size_review_path)
            list(GET _uml_source_size_fields 1 _uml_source_size_review_owner)
            list(GET _uml_source_size_fields 2 _uml_source_size_reviewed_lines)
            list(GET _uml_source_size_fields 3 _uml_source_size_review_note)
            list(GET _uml_source_size_fields 4 _uml_source_size_review_date)
            list(FIND _uml_source_size_review_paths
                "${_uml_source_size_review_path}"
                _uml_source_size_duplicate_index)
            list(FIND _uml_source_size_paths
                "${_uml_source_size_review_path}"
                _uml_source_size_reported_index)

            set(_uml_source_size_record_valid TRUE)
            if(_uml_source_size_review_path STREQUAL "" OR
               IS_ABSOLUTE "${_uml_source_size_review_path}" OR
               _uml_source_size_review_path MATCHES
                   "(^|/)\\.\\.(/|$)" OR
               NOT EXISTS
                   "${_uml_source_size_root}/${_uml_source_size_review_path}" OR
               IS_DIRECTORY
                   "${_uml_source_size_root}/${_uml_source_size_review_path}" OR
               _uml_source_size_reported_index EQUAL -1)
                list(APPEND _uml_source_size_errors
                    "${_uml_source_size_manifest}: stale or unreported path '${_uml_source_size_review_path}'")
                set(_uml_source_size_record_valid FALSE)
            endif()
            if(NOT _uml_source_size_duplicate_index EQUAL -1)
                list(APPEND _uml_source_size_errors
                    "${_uml_source_size_manifest}: duplicate path '${_uml_source_size_review_path}'")
                set(_uml_source_size_record_valid FALSE)
            endif()
            if(_uml_source_size_review_owner STREQUAL "" OR
               _uml_source_size_review_note STREQUAL "" OR
               NOT (_uml_source_size_reviewed_lines STREQUAL "-" OR
                    _uml_source_size_reviewed_lines MATCHES "^[0-9]+$") OR
               NOT (_uml_source_size_review_date STREQUAL "-" OR
                    _uml_source_size_review_date MATCHES
                        "^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$"))
                list(APPEND _uml_source_size_errors
                    "${_uml_source_size_manifest}: invalid review metadata '${_uml_source_size_row}'")
                set(_uml_source_size_record_valid FALSE)
            endif()

            if(_uml_source_size_record_valid)
                list(APPEND _uml_source_size_review_paths
                    "${_uml_source_size_review_path}")
                list(APPEND _uml_source_size_review_owners
                    "${_uml_source_size_review_owner}")
                list(APPEND _uml_source_size_review_lines
                    "${_uml_source_size_reviewed_lines}")
                list(APPEND _uml_source_size_review_notes
                    "${_uml_source_size_review_note}")
                list(APPEND _uml_source_size_review_dates
                    "${_uml_source_size_review_date}")
            endif()
        endforeach()
        if(NOT _uml_source_size_header_seen)
            list(APPEND _uml_source_size_errors
                "${_uml_source_size_manifest}: review manifest has no header")
        endif()
    endif()

    set(_uml_source_size_report
        "category\tguideline\tactual_lines\tover_by\tpath\treview_status\towner\treviewed_lines\treviewed\tnote\n")
    set(_uml_source_size_total 0)
    set(_uml_source_size_above 0)
    set(_uml_source_size_unreviewed 0)
    set(_uml_source_size_changed 0)
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

        set(_uml_source_size_owner -)
        set(_uml_source_size_reviewed_lines -)
        set(_uml_source_size_reviewed -)
        set(_uml_source_size_note -)
        list(FIND _uml_source_size_review_paths
            "${_uml_source_size_relative_path}" _uml_source_size_review_index)
        if(NOT _uml_source_size_review_index EQUAL -1)
            list(GET _uml_source_size_review_owners
                ${_uml_source_size_review_index} _uml_source_size_owner)
            list(GET _uml_source_size_review_lines
                ${_uml_source_size_review_index}
                _uml_source_size_reviewed_lines)
            list(GET _uml_source_size_review_dates
                ${_uml_source_size_review_index} _uml_source_size_reviewed)
            list(GET _uml_source_size_review_notes
                ${_uml_source_size_review_index} _uml_source_size_note)
        endif()

        if(_uml_source_size_category STREQUAL "declarative")
            set(_uml_source_size_status informational)
        elseif(NOT _uml_source_size_review_index EQUAL -1 AND
               NOT _uml_source_size_reviewed_lines STREQUAL "-" AND
               NOT _uml_source_size_reviewed_lines EQUAL
                   _uml_source_size_actual_lines)
            set(_uml_source_size_status changed-since-review)
            math(EXPR _uml_source_size_changed
                "${_uml_source_size_changed} + 1")
        elseif(NOT _uml_source_size_review_index EQUAL -1)
            set(_uml_source_size_status reviewed)
        elseif(_uml_source_size_is_above)
            set(_uml_source_size_status above-guideline-unreviewed)
            math(EXPR _uml_source_size_unreviewed
                "${_uml_source_size_unreviewed} + 1")
        else()
            set(_uml_source_size_status within-guideline)
        endif()

        math(EXPR _uml_source_size_total "${_uml_source_size_total} + 1")
        string(APPEND _uml_source_size_report
            "${_uml_source_size_category}\t${_uml_source_size_guideline}\t${_uml_source_size_actual_lines}\t${_uml_source_size_over_by}\t${_uml_source_size_relative_path}\t${_uml_source_size_status}\t${_uml_source_size_owner}\t${_uml_source_size_reviewed_lines}\t${_uml_source_size_reviewed}\t${_uml_source_size_note}\n")

        if(NOT UML_SOURCE_SIZE_REPORT_PATH AND
           (_uml_source_size_is_above OR
            _uml_source_size_status STREQUAL "changed-since-review"))
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
        "Source-size advisory summary: ${_uml_source_size_total} files, ${_uml_source_size_above} above guideline, ${_uml_source_size_unreviewed} unreviewed, ${_uml_source_size_changed} changed since review")

    if(UML_SOURCE_SIZE_OUT_ERRORS)
        set(${UML_SOURCE_SIZE_OUT_ERRORS} "${_uml_source_size_errors}"
            PARENT_SCOPE)
    endif()
endfunction()
