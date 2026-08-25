# SPDX-License-Identifier: Apache-2.0

if(NOT UML_GUEST_16K_PAGES)
    message(FATAL_ERROR "The Android 17 product supports only 16 KiB guest pages")
endif()

set(_uml_android_root "${PROJECT_SOURCE_DIR}/android/aosp-17")
set(_uml_android_kernel_root "${PROJECT_SOURCE_DIR}/kernel/android-6.18")
set(_uml_kernel_common_root "${PROJECT_SOURCE_DIR}/kernel/common")
include("${PROJECT_SOURCE_DIR}/kernel/common/artifacts/KernelArtifact.cmake")
include("${_uml_android_root}/cmake/AndroidRelease.cmake")
set(_uml_android_rootfs_acceptance
    "${_uml_android_root}/metadata/accepted-release.env")
uml_read_artifact_env(
    "${_uml_android_rootfs_acceptance}"
    ANDROID17_ACCEPTED_DIRECTORY_NAME
    _uml_android_rootfs_directory_name)
set(_uml_android_rootfs_default
    "${PROJECT_SOURCE_DIR}/.build/android/releases/${_uml_android_rootfs_directory_name}")
set(UML_ANDROID17_ROOTFS_RELEASE_DIR "${_uml_android_rootfs_default}" CACHE PATH
    "Installed accepted Android 17 rootfs release directory")

uml_import_android17_rootfs_release(
    TARGET uml_android17_rootfs_release
    RELEASE_ROOT "${UML_ANDROID17_ROOTFS_RELEASE_DIR}"
    ACCEPTANCE "${_uml_android_rootfs_acceptance}"
    ANDROID_ROOT "${_uml_android_root}")
set(UML_ANDROID17_ROOTFS_RELEASE_TARGET uml_android17_rootfs_release
    CACHE INTERNAL "Selected read-only Android 17 rootfs release target" FORCE)

set(_uml_android_kernel_acceptance
    "${_uml_android_kernel_root}/artifacts/accepted-16k.env")
uml_read_artifact_env(
    "${_uml_android_kernel_acceptance}"
    UML_KERNEL_ARTIFACT_DIRECTORY_NAME
    _uml_android_kernel_directory_name)
set(_uml_android_kernel_default
    "${PROJECT_SOURCE_DIR}/.build/kernel/artifacts/${_uml_android_kernel_directory_name}")
set(UML_ANDROID17_KERNEL_ARTIFACT_DIR "${_uml_android_kernel_default}" CACHE PATH
    "Installed accepted Android 17 ACK 6.18 kernel artifact directory")

uml_import_kernel_artifact(
    TARGET uml_android17_kernel_artifact
    ARTIFACT_ROOT "${UML_ANDROID17_KERNEL_ARTIFACT_DIR}"
    ACCEPTANCE "${_uml_android_kernel_acceptance}"
    COMMON_ROOT "${_uml_kernel_common_root}"
    LINE_ROOT "${_uml_android_kernel_root}"
    EXPECTED_FAMILY android-6.18
    EXPECTED_PAGE_SIZE 16384)
set(UML_ANDROID17_KERNEL_ARTIFACT_TARGET uml_android17_kernel_artifact
    CACHE INTERNAL "Selected read-only Android 17 ACK artifact target" FORCE)
