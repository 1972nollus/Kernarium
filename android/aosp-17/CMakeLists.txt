# SPDX-License-Identifier: Apache-2.0

if(NOT UML_GUEST_16K_PAGES)
    message(FATAL_ERROR "The Android 17 product supports only 16 KiB guest pages")
endif()

set(_uml_android_kernel_root "${PROJECT_SOURCE_DIR}/kernel/android-6.18")
set(_uml_kernel_common_root "${PROJECT_SOURCE_DIR}/kernel/common")
include("${PROJECT_SOURCE_DIR}/kernel/common/artifacts/KernelArtifact.cmake")

set(_uml_android_kernel_acceptance
    "${_uml_android_kernel_root}/artifacts/accepted-16k.env")
uml_read_artifact_env(
    "${_uml_android_kernel_acceptance}"
    UML_KERNEL_ARTIFACT_DIRECTORY_NAME
    _uml_android_kernel_directory_name)
uml_watch_artifact_inputs("${_uml_android_kernel_acceptance}")
set(_uml_android_kernel_default
    "${PROJECT_SOURCE_DIR}/.build/kernel/artifacts/${_uml_android_kernel_directory_name}")
set(UML_ANDROID17_KERNEL_ARTIFACT_DIR "${_uml_android_kernel_default}" CACHE PATH
    "Installed accepted Android 17 ACK 6.18 kernel artifact directory")

uml_import_kernel_artifact(
    TARGET uml_android17_kernel_artifact
    ARTIFACT_ROOT "${UML_ANDROID17_KERNEL_ARTIFACT_DIR}"
    COMMON_ROOT "${_uml_kernel_common_root}"
    EXPECTED_FAMILY android-6.18
    EXPECTED_RELEASE_SERIES 6.18
    EXPECTED_PAGE_SIZE 16384)
set(UML_ANDROID17_KERNEL_ARTIFACT_TARGET uml_android17_kernel_artifact
    CACHE INTERNAL "Selected read-only Android 17 ACK artifact target" FORCE)
