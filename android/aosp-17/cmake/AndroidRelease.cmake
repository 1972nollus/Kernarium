# SPDX-License-Identifier: Apache-2.0

include_guard(GLOBAL)

function(uml_import_android17_release)
    set(options)
    set(oneValueArgs TARGET RELEASE_ROOT ACCEPTANCE ANDROID_ROOT)
    cmake_parse_arguments(RELEASE "${options}" "${oneValueArgs}" "" ${ARGN})
    foreach(_uml_required IN ITEMS TARGET RELEASE_ROOT ACCEPTANCE ANDROID_ROOT)
        if(NOT RELEASE_${_uml_required})
            message(FATAL_ERROR
                "uml_import_android17_release requires ${_uml_required}")
        endif()
    endforeach()
    if(TARGET "${RELEASE_TARGET}")
        message(FATAL_ERROR "Android release target already exists: ${RELEASE_TARGET}")
    endif()
    if(NOT IS_ABSOLUTE "${RELEASE_RELEASE_ROOT}")
        message(FATAL_ERROR "Android release directory must be absolute")
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_uml_release_verifier "${RELEASE_ANDROID_ROOT}/tools/verify-release.py")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -B "${_uml_release_verifier}"
            --release "${RELEASE_RELEASE_ROOT}"
            --acceptance "${RELEASE_ACCEPTANCE}"
            --installed
        RESULT_VARIABLE _uml_release_result
        OUTPUT_VARIABLE _uml_release_output
        ERROR_VARIABLE _uml_release_error)
    if(NOT _uml_release_result EQUAL 0)
        message(FATAL_ERROR
            "Cannot import the accepted Android 17 release:\n"
            "${_uml_release_output}${_uml_release_error}")
    endif()

    set(_uml_manifest "${RELEASE_RELEASE_ROOT}/release-manifest.json")
    file(READ "${_uml_manifest}" _uml_manifest_json)
    string(JSON _uml_product_family GET "${_uml_manifest_json}" product_family)
    string(JSON _uml_directory_name GET "${_uml_manifest_json}" directory_name)
    string(JSON _uml_release_id GET "${_uml_manifest_json}" release_id)
    string(JSON _uml_android_major GET "${_uml_manifest_json}" android major)
    string(JSON _uml_kernel_family GET "${_uml_manifest_json}" kernel family)
    string(JSON _uml_kernel_release GET "${_uml_manifest_json}" kernel release)
    string(JSON _uml_kernel_page_size GET "${_uml_manifest_json}" kernel page_size)
    if(NOT _uml_product_family STREQUAL "android-17-ack-6.18-16k" OR
       NOT _uml_android_major EQUAL 17 OR
       NOT _uml_kernel_family STREQUAL "android-6.18" OR
       NOT _uml_kernel_page_size EQUAL 16384)
        message(FATAL_ERROR "Imported release is not the Android 17 ACK 6.18 16 KiB product")
    endif()

    add_library("${RELEASE_TARGET}" INTERFACE)
    set_property(TARGET "${RELEASE_TARGET}" PROPERTY
        UML_ARTIFACT_FAMILY "${_uml_product_family}")
    set_property(TARGET "${RELEASE_TARGET}" PROPERTY
        UML_ARTIFACT_DIRECTORY "${RELEASE_RELEASE_ROOT}")
    set_property(TARGET "${RELEASE_TARGET}" PROPERTY
        UML_ARTIFACT_DIRECTORY_NAME "${_uml_directory_name}")
    set_property(TARGET "${RELEASE_TARGET}" PROPERTY
        UML_ARTIFACT_RELEASE_ID "${_uml_release_id}")
    set_property(TARGET "${RELEASE_TARGET}" PROPERTY
        UML_KERNEL_RELEASE "${_uml_kernel_release}")
    set_property(TARGET "${RELEASE_TARGET}" PROPERTY
        UML_KERNEL_PAGE_SIZE "${_uml_kernel_page_size}")
    set_property(TARGET "${RELEASE_TARGET}" PROPERTY
        UML_KERNEL_IMAGE "${RELEASE_RELEASE_ROOT}/kernel/vmlinux")
    set_property(TARGET "${RELEASE_TARGET}" PROPERTY
        UML_KERNEL_CONFIG "${RELEASE_RELEASE_ROOT}/kernel/config")
    set_property(TARGET "${RELEASE_TARGET}" PROPERTY
        UML_KERNEL_MODULES_BUILTIN
        "${RELEASE_RELEASE_ROOT}/kernel/modules.builtin")
    set_property(TARGET "${RELEASE_TARGET}" PROPERTY
        UML_ANDROID_RELEASE_MANIFEST "${_uml_manifest}")
    set_property(TARGET "${RELEASE_TARGET}" PROPERTY
        UML_ANDROID_KERNEL_COMMAND_LINE
        "${RELEASE_RELEASE_ROOT}/boot/kernel-command-line.txt")
    set_property(TARGET "${RELEASE_TARGET}" PROPERTY
        UML_ANDROID_BASE_DISK
        "${RELEASE_RELEASE_ROOT}/guest/android-17-uml-arm64.img")

    set(_uml_watch_inputs
        "${RELEASE_ACCEPTANCE}"
        "${_uml_manifest}"
        "${_uml_release_verifier}"
        "${RELEASE_ANDROID_ROOT}/tools/release_common.py"
        "${RELEASE_ANDROID_ROOT}/build/verify-kernel-elf.py"
        "${RELEASE_ANDROID_ROOT}/tools/verify-initramfs.py"
        "${RELEASE_ANDROID_ROOT}/tools/boot-image.py"
        "${RELEASE_ANDROID_ROOT}/tools/ubd-disk.py"
        "${RELEASE_RELEASE_ROOT}/kernel/vmlinux"
        "${RELEASE_RELEASE_ROOT}/kernel/config"
        "${RELEASE_RELEASE_ROOT}/kernel/modules.builtin"
        "${RELEASE_RELEASE_ROOT}/boot/kernel-command-line.txt"
        "${RELEASE_RELEASE_ROOT}/guest/android-17-uml-arm64.img")
    foreach(_uml_input IN LISTS _uml_watch_inputs)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${_uml_input}")
    endforeach()
endfunction()
