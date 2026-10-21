# SPDX-License-Identifier: GPL-2.0-or-later

set(UML_KERNEL_COMMON_ROOT "${PROJECT_SOURCE_DIR}/kernel/common")
set(UML_KERNEL_LINE_ROOT "${PROJECT_SOURCE_DIR}/kernel/mainline-6.18")
include("${UML_KERNEL_COMMON_ROOT}/artifacts/KernelArtifact.cmake")

set(UML_KERNEL_PRODUCT "mainline-6.18" CACHE STRING
    "Prebuilt kernel product consumed by the selected Apple executable/runtime")
set_property(CACHE UML_KERNEL_PRODUCT PROPERTY STRINGS
    mainline-6.18 android-17)

if(UML_KERNEL_PRODUCT STREQUAL "android-17")
    # Apple targets consume the Android kernel artifact only. Rootfs bundles
    # are selected at launch and are not inputs to the emulator build.
    add_subdirectory("${PROJECT_SOURCE_DIR}/android/lineage-24"
        "${CMAKE_CURRENT_BINARY_DIR}/android-lineage-24")
    set(UML_ACTIVE_KERNEL_ARTIFACT_TARGET
        "${UML_ANDROID17_KERNEL_ARTIFACT_TARGET}"
        CACHE INTERNAL "Selected read-only kernel artifact target" FORCE)
    return()
endif()

if(NOT UML_KERNEL_PRODUCT STREQUAL "mainline-6.18")
    message(FATAL_ERROR
        "UML_KERNEL_PRODUCT must be mainline-6.18 or android-17")
endif()

# The root rejects every slice other than arm64 and x86_64. The descriptor
# only names the bundle; the import verifies the bundle's own slice against
# this configuration.
if(UML_MAINLINE_KERNEL_SLICE STREQUAL "x86_64")
    set(_uml_mainline_acceptance
        "${UML_KERNEL_LINE_ROOT}/slices/x86_64/artifacts/accepted-4k.env")
else()
    set(_uml_mainline_acceptance
        "${UML_KERNEL_LINE_ROOT}/artifacts/accepted-4k.env")
endif()
uml_read_artifact_env(
    "${_uml_mainline_acceptance}"
    UML_KERNEL_ARTIFACT_DIRECTORY_NAME
    _uml_mainline_directory_name)
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
    EXPECTED_RELEASE_SERIES 6.18)

set(UML_ACTIVE_KERNEL_ARTIFACT_TARGET uml_mainline_kernel_artifact
    CACHE INTERNAL "Selected read-only kernel artifact target" FORCE)
