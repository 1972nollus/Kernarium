# SPDX-License-Identifier: Apache-2.0

if(NOT UML_GUEST_16K_PAGES)
    message(FATAL_ERROR "The Android 17 product supports only 16 KiB guest pages")
endif()

set(_uml_android_root "${PROJECT_SOURCE_DIR}/android/aosp-17")
include("${PROJECT_SOURCE_DIR}/kernel/common/artifacts/KernelArtifact.cmake")
include("${_uml_android_root}/cmake/AndroidRelease.cmake")
set(_uml_android_acceptance
    "${_uml_android_root}/metadata/accepted-release.env")
uml_read_artifact_env(
    "${_uml_android_acceptance}"
    ANDROID17_ACCEPTED_DIRECTORY_NAME
    _uml_android_directory_name)
set(_uml_android_release_default
    "${PROJECT_SOURCE_DIR}/.build/android/releases/${_uml_android_directory_name}")
set(UML_ANDROID17_RELEASE_DIR "${_uml_android_release_default}" CACHE PATH
    "Installed accepted Android 17 ACK 6.18 16 KiB release directory")

uml_import_android17_release(
    TARGET uml_android17_release
    RELEASE_ROOT "${UML_ANDROID17_RELEASE_DIR}"
    ACCEPTANCE "${_uml_android_acceptance}"
    ANDROID_ROOT "${_uml_android_root}")
set(UML_ANDROID17_RELEASE_TARGET uml_android17_release
    CACHE INTERNAL "Selected read-only Android 17 release target" FORCE)
