# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

function(uml_read_artifact_env METADATA KEY OUT_VALUE)
    if(NOT IS_ABSOLUTE "${METADATA}" OR NOT EXISTS "${METADATA}")
        message(FATAL_ERROR "Artifact metadata is missing: ${METADATA}")
    endif()
    file(STRINGS "${METADATA}" _uml_artifact_rows
        REGEX "^${KEY}=(('[^']*')|([0-9]+))$")
    list(LENGTH _uml_artifact_rows _uml_artifact_row_count)
    if(NOT _uml_artifact_row_count EQUAL 1)
        message(FATAL_ERROR
            "Artifact metadata does not contain exactly one ${KEY}: ${METADATA}")
    endif()
    list(GET _uml_artifact_rows 0 _uml_artifact_row)
    string(REGEX REPLACE "^[^=]+='?([^']*)'?$" "\\1"
        _uml_artifact_value "${_uml_artifact_row}")
    set(${OUT_VALUE} "${_uml_artifact_value}" PARENT_SCOPE)
endfunction()

function(uml_watch_artifact_inputs)
    foreach(_uml_artifact_input IN LISTS ARGN)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${_uml_artifact_input}")
    endforeach()
endfunction()

function(uml_cache_accepted_artifact_path VARIABLE DEFAULT_VALUE MANAGED_ROOT DESCRIPTION)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs LEGACY_DEFAULTS)
    cmake_parse_arguments(ARTIFACT_CACHE
        "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(ARTIFACT_CACHE_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown accepted-artifact cache arguments: "
            "${ARTIFACT_CACHE_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT IS_ABSOLUTE "${DEFAULT_VALUE}" OR NOT IS_ABSOLUTE "${MANAGED_ROOT}")
        message(FATAL_ERROR
            "Accepted artifact defaults and managed roots must be absolute")
    endif()

    set(_uml_managed_artifact_root "${MANAGED_ROOT}")
    cmake_path(IS_PREFIX _uml_managed_artifact_root "${DEFAULT_VALUE}"
        NORMALIZE _uml_default_is_managed)
    if(NOT _uml_default_is_managed)
        message(FATAL_ERROR
            "Accepted artifact defaults must be below the managed root")
    endif()
    foreach(_uml_legacy_default IN LISTS ARTIFACT_CACHE_LEGACY_DEFAULTS)
        if(NOT IS_ABSOLUTE "${_uml_legacy_default}")
            message(FATAL_ERROR
                "Legacy accepted-artifact defaults must be absolute")
        endif()
        set(_uml_legacy_artifact_path "${_uml_legacy_default}")
        cmake_path(IS_PREFIX _uml_managed_artifact_root
            "${_uml_legacy_artifact_path}"
            NORMALIZE _uml_legacy_default_is_managed)
        if(NOT _uml_legacy_default_is_managed)
            message(FATAL_ERROR
                "Legacy accepted-artifact defaults must be below the managed root")
        endif()
    endforeach()

    set(_uml_default_tracker "${VARIABLE}_ACCEPTED_DEFAULT")
    set(_uml_follow_tracker "${VARIABLE}_FOLLOW_ACCEPTED")
    set(_uml_had_default_tracker OFF)
    if(DEFINED ${_uml_default_tracker})
        set(_uml_had_default_tracker ON)
        set(_uml_previous_default "${${_uml_default_tracker}}")
    endif()

    if(DEFINED ${_uml_follow_tracker})
        if(NOT "${${_uml_follow_tracker}}" STREQUAL "ON" AND
           NOT "${${_uml_follow_tracker}}" STREQUAL "OFF")
            message(FATAL_ERROR
                "Accepted artifact follow state must be ON or OFF")
        endif()
        set(_uml_follow_accepted "${${_uml_follow_tracker}}")
    elseif(_uml_had_default_tracker)
        # Migrate the equality-only tracker used by earlier build trees.
        if(NOT DEFINED ${VARIABLE} OR "${${VARIABLE}}" STREQUAL "" OR
           "${${VARIABLE}}" STREQUAL "${_uml_previous_default}")
            set(_uml_follow_accepted ON)
        else()
            set(_uml_follow_accepted OFF)
        endif()
    else()
        set(_uml_recognized_legacy_default OFF)
        foreach(_uml_legacy_default IN LISTS ARTIFACT_CACHE_LEGACY_DEFAULTS)
            if(DEFINED ${VARIABLE} AND
               "${${VARIABLE}}" STREQUAL "${_uml_legacy_default}")
                set(_uml_recognized_legacy_default ON)
            endif()
        endforeach()
        if(NOT DEFINED ${VARIABLE} OR "${${VARIABLE}}" STREQUAL "" OR
           "${${VARIABLE}}" STREQUAL "${DEFAULT_VALUE}" OR
           _uml_recognized_legacy_default)
            # A trackerless value follows only when it is empty, already names
            # the current default, or matches the caller's finite legacy list.
            set(_uml_follow_accepted ON)
        else()
            set(_uml_follow_accepted OFF)
        endif()
    endif()

    if(_uml_follow_accepted AND _uml_had_default_tracker AND
       DEFINED ${VARIABLE} AND NOT "${${VARIABLE}}" STREQUAL "" AND
       NOT "${${VARIABLE}}" STREQUAL "${_uml_previous_default}")
        # A caller replaced a previously generated default. Remember that it
        # is pinned even if a future accepted default happens to equal it.
        set(_uml_follow_accepted OFF)
    endif()

    if(NOT DEFINED ${VARIABLE} OR "${${VARIABLE}}" STREQUAL "")
        set(_uml_follow_accepted ON)
    endif()
    if(_uml_follow_accepted)
        set(_uml_selected_artifact "${DEFAULT_VALUE}")
    else()
        set(_uml_selected_artifact "${${VARIABLE}}")
    endif()

    set(${VARIABLE} "${_uml_selected_artifact}"
        CACHE PATH "${DESCRIPTION}" FORCE)
    set(${_uml_default_tracker} "${DEFAULT_VALUE}" CACHE INTERNAL
        "Last generated accepted-artifact default for ${VARIABLE}" FORCE)
    set(${_uml_follow_tracker} "${_uml_follow_accepted}" CACHE INTERNAL
        "Whether ${VARIABLE} follows the accepted-artifact default" FORCE)
    set(${VARIABLE} "${_uml_selected_artifact}" PARENT_SCOPE)
endfunction()

function(uml_import_kernel_artifact)
    set(options)
    set(oneValueArgs
        TARGET ARTIFACT_ROOT COMMON_ROOT EXPECTED_FAMILY EXPECTED_SLICE
        EXPECTED_RELEASE_SERIES EXPECTED_PAGE_SIZE)
    cmake_parse_arguments(ARTIFACT "${options}" "${oneValueArgs}" "" ${ARGN})
    foreach(_uml_required IN ITEMS
            TARGET ARTIFACT_ROOT COMMON_ROOT EXPECTED_FAMILY EXPECTED_SLICE
            EXPECTED_RELEASE_SERIES EXPECTED_PAGE_SIZE)
        if(NOT ARTIFACT_${_uml_required})
            message(FATAL_ERROR
                "uml_import_kernel_artifact requires ${_uml_required}")
        endif()
    endforeach()
    if(TARGET "${ARTIFACT_TARGET}")
        message(FATAL_ERROR "Kernel artifact target already exists: ${ARTIFACT_TARGET}")
    endif()
    if(NOT IS_ABSOLUTE "${ARTIFACT_ARTIFACT_ROOT}")
        message(FATAL_ERROR "Kernel artifact root must be absolute")
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_uml_abi_metadata "${ARTIFACT_COMMON_ROOT}/abi/identity.env")
    set(_uml_verifier
        "${ARTIFACT_COMMON_ROOT}/artifacts/verify-kernel-artifact.py")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -B "${_uml_verifier}"
            --artifact "${ARTIFACT_ARTIFACT_ROOT}"
            --common-root "${ARTIFACT_COMMON_ROOT}"
            --abi-metadata "${_uml_abi_metadata}"
            --compatible
            --expected-family "${ARTIFACT_EXPECTED_FAMILY}"
            --expected-slice "${ARTIFACT_EXPECTED_SLICE}"
            --expected-release-series "${ARTIFACT_EXPECTED_RELEASE_SERIES}"
            --expected-page-size "${ARTIFACT_EXPECTED_PAGE_SIZE}"
        RESULT_VARIABLE _uml_artifact_result
        OUTPUT_VARIABLE _uml_artifact_output
        ERROR_VARIABLE _uml_artifact_error)
    if(NOT _uml_artifact_result EQUAL 0)
        message(FATAL_ERROR
            "Cannot import the compatible kernel artifact:\n"
            "${_uml_artifact_output}${_uml_artifact_error}")
    endif()

    set(_uml_manifest "${ARTIFACT_ARTIFACT_ROOT}/kernel-artifact.json")
    file(READ "${_uml_manifest}" _uml_manifest_json)
    string(JSON _uml_family GET "${_uml_manifest_json}" family)
    string(JSON _uml_slice GET "${_uml_manifest_json}" slice)
    string(JSON _uml_page_size GET "${_uml_manifest_json}" page_size)
    string(JSON _uml_release_id GET "${_uml_manifest_json}" release_id)
    string(JSON _uml_directory_name GET "${_uml_manifest_json}" directory_name)
    if(NOT _uml_family STREQUAL "${ARTIFACT_EXPECTED_FAMILY}")
        message(FATAL_ERROR
            "Imported kernel family ${_uml_family} does not match "
            "${ARTIFACT_EXPECTED_FAMILY}")
    endif()
    if(NOT _uml_slice STREQUAL "${ARTIFACT_EXPECTED_SLICE}")
        message(FATAL_ERROR
            "Imported kernel slice ${_uml_slice} does not match "
            "${ARTIFACT_EXPECTED_SLICE}")
    endif()
    if(NOT _uml_page_size EQUAL "${ARTIFACT_EXPECTED_PAGE_SIZE}")
        message(FATAL_ERROR
            "Imported kernel page size ${_uml_page_size} does not match "
            "the configured ${ARTIFACT_EXPECTED_PAGE_SIZE}")
    endif()

    add_library("${ARTIFACT_TARGET}" INTERFACE)
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_ARTIFACT_FAMILY "${_uml_family}")
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_ARTIFACT_SLICE "${_uml_slice}")
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_ARTIFACT_DIRECTORY "${ARTIFACT_ARTIFACT_ROOT}")
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_ARTIFACT_DIRECTORY_NAME "${_uml_directory_name}")
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_ARTIFACT_RELEASE_ID "${_uml_release_id}")
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_KERNEL_PAGE_SIZE "${_uml_page_size}")
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_KERNEL_IMAGE "${ARTIFACT_ARTIFACT_ROOT}/kernel/vmlinux")
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_KERNEL_CONFIG "${ARTIFACT_ARTIFACT_ROOT}/kernel/config")
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_KERNEL_MODULES_BUILTIN
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/modules.builtin")

    uml_watch_artifact_inputs(
        "${_uml_manifest}"
        "${_uml_abi_metadata}"
        "${_uml_verifier}"
        "${ARTIFACT_COMMON_ROOT}/artifacts/kernel_artifact.py"
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/vmlinux"
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/config"
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/kernel.release"
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/modules.builtin"
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/modules.builtin.modinfo")
endfunction()
