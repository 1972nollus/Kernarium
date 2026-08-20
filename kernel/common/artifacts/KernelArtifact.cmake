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

function(uml_import_mainline_kernel_artifact)
    set(options)
    set(oneValueArgs
        TARGET ARTIFACT_ROOT ACCEPTANCE COMMON_ROOT LINE_ROOT EXPECTED_PAGE_SIZE)
    cmake_parse_arguments(ARTIFACT "${options}" "${oneValueArgs}" "" ${ARGN})
    foreach(_uml_required IN ITEMS
            TARGET ARTIFACT_ROOT ACCEPTANCE COMMON_ROOT LINE_ROOT EXPECTED_PAGE_SIZE)
        if(NOT ARTIFACT_${_uml_required})
            message(FATAL_ERROR
                "uml_import_mainline_kernel_artifact requires ${_uml_required}")
        endif()
    endforeach()
    if(TARGET "${ARTIFACT_TARGET}")
        message(FATAL_ERROR "Kernel artifact target already exists: ${ARTIFACT_TARGET}")
    endif()
    if(NOT IS_ABSOLUTE "${ARTIFACT_ARTIFACT_ROOT}")
        message(FATAL_ERROR "Kernel artifact root must be absolute")
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_uml_kernel_metadata "${ARTIFACT_LINE_ROOT}/patches/metadata.env")
    set(_uml_patch_manifest "${ARTIFACT_LINE_ROOT}/patches/series/manifest.tsv")
    set(_uml_abi_metadata "${ARTIFACT_COMMON_ROOT}/abi/identity.env")
    set(_uml_verifier
        "${ARTIFACT_COMMON_ROOT}/artifacts/verify-kernel-artifact.py")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -B "${_uml_verifier}"
            --artifact "${ARTIFACT_ARTIFACT_ROOT}"
            --common-root "${ARTIFACT_COMMON_ROOT}"
            --kernel-metadata "${_uml_kernel_metadata}"
            --patch-manifest "${_uml_patch_manifest}"
            --abi-metadata "${_uml_abi_metadata}"
            --acceptance "${ARTIFACT_ACCEPTANCE}"
        RESULT_VARIABLE _uml_artifact_result
        OUTPUT_VARIABLE _uml_artifact_output
        ERROR_VARIABLE _uml_artifact_error)
    if(NOT _uml_artifact_result EQUAL 0)
        message(FATAL_ERROR
            "Cannot import the accepted mainline kernel artifact:\n"
            "${_uml_artifact_output}${_uml_artifact_error}")
    endif()

    set(_uml_manifest "${ARTIFACT_ARTIFACT_ROOT}/kernel-artifact.json")
    file(READ "${_uml_manifest}" _uml_manifest_json)
    string(JSON _uml_family GET "${_uml_manifest_json}" family)
    string(JSON _uml_page_size GET "${_uml_manifest_json}" page_size)
    string(JSON _uml_release GET "${_uml_manifest_json}" release)
    string(JSON _uml_release_id GET "${_uml_manifest_json}" release_id)
    string(JSON _uml_directory_name GET "${_uml_manifest_json}" directory_name)
    if(NOT _uml_family STREQUAL "mainline-6.18")
        message(FATAL_ERROR "Imported kernel family is not mainline-6.18")
    endif()
    if(NOT _uml_page_size EQUAL ARTIFACT_EXPECTED_PAGE_SIZE)
        message(FATAL_ERROR
            "Imported mainline kernel page size ${_uml_page_size} does not match "
            "the configured ${ARTIFACT_EXPECTED_PAGE_SIZE}")
    endif()

    add_library("${ARTIFACT_TARGET}" INTERFACE)
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_ARTIFACT_FAMILY "${_uml_family}")
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_ARTIFACT_DIRECTORY "${ARTIFACT_ARTIFACT_ROOT}")
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_ARTIFACT_DIRECTORY_NAME "${_uml_directory_name}")
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_ARTIFACT_RELEASE_ID "${_uml_release_id}")
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_KERNEL_RELEASE "${_uml_release}")
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
        "${ARTIFACT_ACCEPTANCE}"
        "${_uml_manifest}"
        "${_uml_kernel_metadata}"
        "${_uml_patch_manifest}"
        "${_uml_abi_metadata}"
        "${_uml_verifier}"
        "${ARTIFACT_COMMON_ROOT}/artifacts/kernel_artifact.py"
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/vmlinux"
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/config"
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/kernel.release"
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/modules.builtin"
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/modules.builtin.modinfo")
endfunction()
