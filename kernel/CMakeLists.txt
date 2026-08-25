# SPDX-License-Identifier: GPL-2.0-or-later

set(UML_KERNEL_COMMON_ROOT "${PROJECT_SOURCE_DIR}/kernel/common")
set(UML_KERNEL_LINE_ROOT "${PROJECT_SOURCE_DIR}/kernel/mainline-6.18")
set(UML_ANDROID_KERNEL_LINE_ROOT "${PROJECT_SOURCE_DIR}/kernel/android-6.18")
include("${UML_KERNEL_COMMON_ROOT}/artifacts/KernelArtifact.cmake")

set(UML_KERNEL_PRODUCT "mainline-6.18" CACHE STRING
    "Prebuilt kernel product consumed by the selected Apple executable/runtime")
set_property(CACHE UML_KERNEL_PRODUCT PROPERTY STRINGS mainline-6.18 android-17)

if(UML_KERNEL_PRODUCT STREQUAL "android-17")
    # The ACK kernel and Android rootfs are independent accepted inputs. The
    # application may advance its signed kernel carrier without republishing
    # or invalidating an otherwise compatible guest disk bundle.
    if(NOT UML_GUEST_16K_PAGES)
        message(FATAL_ERROR
            "The Android 17 product supports only 16 KiB guest pages")
    endif()
    if(NOT TARGET uml_android17_kernel_artifact OR
       NOT TARGET uml_android17_rootfs_release)
        add_subdirectory("${PROJECT_SOURCE_DIR}/android/aosp-17"
            "${CMAKE_CURRENT_BINARY_DIR}/android-aosp-17")
    endif()
    set(UML_ACTIVE_KERNEL_ARTIFACT_TARGET
        "${UML_ANDROID17_KERNEL_ARTIFACT_TARGET}"
        CACHE INTERNAL "Selected read-only kernel artifact target" FORCE)
    return()
endif()

if(NOT UML_KERNEL_PRODUCT STREQUAL "mainline-6.18")
    message(FATAL_ERROR
        "UML_KERNEL_PRODUCT must be mainline-6.18 or android-17")
endif()

if(UML_GUEST_16K_PAGES)
    set(_uml_mainline_acceptance
        "${UML_KERNEL_LINE_ROOT}/artifacts/accepted-16k.env")
    set(_uml_expected_page_size 16384)
else()
    set(_uml_mainline_acceptance
        "${UML_KERNEL_LINE_ROOT}/artifacts/accepted-4k.env")
    set(_uml_expected_page_size 4096)
endif()
uml_read_artifact_env(
    "${_uml_mainline_acceptance}"
    UML_KERNEL_ARTIFACT_DIRECTORY_NAME
    _uml_mainline_directory_name)
set(_uml_mainline_artifact_default
    "${PROJECT_SOURCE_DIR}/.build/kernel/artifacts/${_uml_mainline_directory_name}")
set(UML_MAINLINE_618_KERNEL_ARTIFACT_DIR
    "${_uml_mainline_artifact_default}" CACHE PATH
    "Installed accepted mainline-6.18 kernel artifact directory")

uml_import_kernel_artifact(
    TARGET uml_mainline_kernel_artifact
    ARTIFACT_ROOT "${UML_MAINLINE_618_KERNEL_ARTIFACT_DIR}"
    ACCEPTANCE "${_uml_mainline_acceptance}"
    COMMON_ROOT "${UML_KERNEL_COMMON_ROOT}"
    LINE_ROOT "${UML_KERNEL_LINE_ROOT}"
    EXPECTED_FAMILY mainline-6.18
    EXPECTED_PAGE_SIZE "${_uml_expected_page_size}")

set(UML_ACTIVE_KERNEL_ARTIFACT_TARGET uml_mainline_kernel_artifact
    CACHE INTERNAL "Selected read-only kernel artifact target" FORCE)
