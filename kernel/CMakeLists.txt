# SPDX-License-Identifier: GPL-2.0-or-later

set(UML_KERNEL_COMMON_ROOT "${PROJECT_SOURCE_DIR}/kernel/common")
set(UML_KERNEL_LINE_ROOT "${PROJECT_SOURCE_DIR}/kernel/mainline-6.18")
set(UML_ANDROID_KERNEL_LINE_ROOT "${PROJECT_SOURCE_DIR}/kernel/android-6.18")
include("${UML_KERNEL_COMMON_ROOT}/artifacts/KernelArtifact.cmake")

set(UML_KERNEL_PRODUCT "mainline-6.18" CACHE STRING
    "Prebuilt kernel product consumed by the selected Apple executable/runtime")
set_property(CACHE UML_KERNEL_PRODUCT PROPERTY STRINGS
    mainline-6.18 mainline-6.18-vulkan-experiment android-17)

if(UML_KERNEL_PRODUCT STREQUAL "android-17")
    # Apple targets consume the ACK artifact only. Rootfs bundles are selected
    # at launch and are not inputs to the emulator build.
    if(NOT UML_GUEST_16K_PAGES)
        message(FATAL_ERROR
            "The Android 17 product supports only 16 KiB guest pages")
    endif()
    if(NOT TARGET uml_android17_kernel_artifact)
        add_subdirectory("${PROJECT_SOURCE_DIR}/android/aosp-17"
            "${CMAKE_CURRENT_BINARY_DIR}/android-aosp-17")
    endif()
    set(UML_ACTIVE_KERNEL_ARTIFACT_TARGET
        "${UML_ANDROID17_KERNEL_ARTIFACT_TARGET}"
        CACHE INTERNAL "Selected read-only kernel artifact target" FORCE)
    return()
endif()

if(NOT UML_KERNEL_PRODUCT STREQUAL "mainline-6.18" AND
   NOT UML_KERNEL_PRODUCT STREQUAL "mainline-6.18-vulkan-experiment")
    message(FATAL_ERROR
        "UML_KERNEL_PRODUCT must be mainline-6.18, "
        "mainline-6.18-vulkan-experiment, or android-17")
endif()

set(UML_MAINLINE_KERNEL_SLICE "arm64" CACHE STRING
    "Mainline kernel userspace ISA slice")
set_property(CACHE UML_MAINLINE_KERNEL_SLICE PROPERTY STRINGS arm64 x86_64)
if(NOT UML_MAINLINE_KERNEL_SLICE STREQUAL "arm64" AND
   NOT UML_MAINLINE_KERNEL_SLICE STREQUAL "x86_64")
    message(FATAL_ERROR
        "UML_MAINLINE_KERNEL_SLICE must be arm64 or x86_64")
endif()
if(UML_KERNEL_PRODUCT STREQUAL "mainline-6.18-vulkan-experiment" AND
   NOT UML_MAINLINE_KERNEL_SLICE STREQUAL "arm64")
    message(FATAL_ERROR
        "The mainline-6.18-vulkan-experiment product is published only for "
        "the arm64 slice")
endif()

# The Vulkan experiment reads the same mainline-6.18 series through its own
# fixed descriptors.
if(UML_MAINLINE_KERNEL_SLICE STREQUAL "x86_64")
    if(UML_GUEST_16K_PAGES)
        message(FATAL_ERROR
            "The x86_64 mainline kernel slice supports only 4 KiB guest pages")
    endif()
    set(_uml_mainline_acceptance
        "${UML_KERNEL_LINE_ROOT}/slices/x86_64/artifacts/accepted-4k.env")
    set(_uml_expected_page_size 4096)
elseif(UML_GUEST_16K_PAGES)
    if(UML_KERNEL_PRODUCT STREQUAL "mainline-6.18-vulkan-experiment")
        set(_uml_mainline_acceptance
            "${UML_KERNEL_LINE_ROOT}/artifacts/vulkan-experiment-16k.env")
    else()
        set(_uml_mainline_acceptance
            "${UML_KERNEL_LINE_ROOT}/artifacts/accepted-16k.env")
    endif()
    set(_uml_expected_page_size 16384)
else()
    if(UML_KERNEL_PRODUCT STREQUAL "mainline-6.18-vulkan-experiment")
        set(_uml_mainline_acceptance
            "${UML_KERNEL_LINE_ROOT}/artifacts/vulkan-experiment-4k.env")
    else()
        set(_uml_mainline_acceptance
            "${UML_KERNEL_LINE_ROOT}/artifacts/accepted-4k.env")
    endif()
    set(_uml_expected_page_size 4096)
endif()
uml_read_artifact_env(
    "${_uml_mainline_acceptance}"
    UML_KERNEL_ARTIFACT_DIRECTORY_NAME
    _uml_mainline_directory_name)
uml_read_artifact_env(
    "${_uml_mainline_acceptance}"
    UML_KERNEL_ARTIFACT_SLICE
    _uml_mainline_accepted_slice)
if(NOT "${_uml_mainline_accepted_slice}" STREQUAL
       "${UML_MAINLINE_KERNEL_SLICE}")
    message(FATAL_ERROR
        "Accepted mainline kernel slice ${_uml_mainline_accepted_slice} does "
        "not match configured ${UML_MAINLINE_KERNEL_SLICE}")
endif()
uml_watch_artifact_inputs("${_uml_mainline_acceptance}")
set(_uml_mainline_artifact_default
    "${PROJECT_SOURCE_DIR}/.build/kernel/artifacts/${_uml_mainline_directory_name}")
uml_cache_accepted_artifact_path(
    UML_MAINLINE_618_KERNEL_ARTIFACT_DIR
    "${_uml_mainline_artifact_default}"
    "Installed accepted mainline-6.18 kernel artifact directory")

uml_import_kernel_artifact(
    TARGET uml_mainline_kernel_artifact
    ARTIFACT_ROOT "${UML_MAINLINE_618_KERNEL_ARTIFACT_DIR}"
    COMMON_ROOT "${UML_KERNEL_COMMON_ROOT}"
    EXPECTED_FAMILY mainline-6.18
    EXPECTED_SLICE "${UML_MAINLINE_KERNEL_SLICE}"
    EXPECTED_RELEASE_SERIES 6.18
    EXPECTED_PAGE_SIZE "${_uml_expected_page_size}")

set(UML_ACTIVE_KERNEL_ARTIFACT_TARGET uml_mainline_kernel_artifact
    CACHE INTERNAL "Selected read-only kernel artifact target" FORCE)
