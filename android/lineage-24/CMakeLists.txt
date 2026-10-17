# SPDX-License-Identifier: Apache-2.0

set(_uml_android_kernel_root "${PROJECT_SOURCE_DIR}/kernel/mainline-6.18")
set(_uml_kernel_common_root "${PROJECT_SOURCE_DIR}/kernel/common")
include("${PROJECT_SOURCE_DIR}/kernel/common/artifacts/KernelArtifact.cmake")

# The descriptor only names the bundle; the import verifies the bundle's own
# family, slice, release series and page size.
set(_uml_android_kernel_acceptance
    "${_uml_android_kernel_root}/artifacts/accepted-android-4k.env")
uml_read_artifact_env(
    "${_uml_android_kernel_acceptance}"
    UML_KERNEL_ARTIFACT_DIRECTORY_NAME
    _uml_android_kernel_directory_name)
uml_watch_artifact_inputs("${_uml_android_kernel_acceptance}")
set(_uml_android_kernel_default
    "${PROJECT_SOURCE_DIR}/.build/kernel/artifacts/${_uml_android_kernel_directory_name}")
uml_cache_accepted_artifact_path(
    UML_ANDROID17_KERNEL_ARTIFACT_DIR
    "${_uml_android_kernel_default}"
    "Installed accepted Android 17 kernel artifact directory")

uml_import_kernel_artifact(
    TARGET uml_android17_kernel_artifact
    ARTIFACT_ROOT "${UML_ANDROID17_KERNEL_ARTIFACT_DIR}"
    COMMON_ROOT "${_uml_kernel_common_root}"
    EXPECTED_FAMILY android-6.18
    EXPECTED_SLICE arm64
    EXPECTED_RELEASE_SERIES 6.18
    EXPECTED_PAGE_SIZE 4096)
set(UML_ANDROID17_KERNEL_ARTIFACT_TARGET uml_android17_kernel_artifact
    CACHE INTERNAL "Selected read-only Android 17 kernel artifact target" FORCE)
