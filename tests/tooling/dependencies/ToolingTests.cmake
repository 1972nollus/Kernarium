# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME dependency-source-preparer
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_prepare.sh
    LABELS dependency
    # The paired Vulkan coverage prepares two further fixture repositories and
    # replays their series through several rejection cases.
    TIMEOUT 180)

uml_add_tooling_test(
    NAME dependency-patch-regenerator
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_rebase_patches.sh
    LABELS dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME dependency-build-manifest
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_build_manifest.sh
    LABELS dependency
    # Six manifest checks each authenticate every generated patch byte, and the
    # experimental Vulkan product adds two large series to that inventory.
    TIMEOUT 180)

uml_add_tooling_test(
    NAME dependency-product-manifest-verifier
    COMMAND ${CMAKE_COMMAND}
        -DVERIFY_SCRIPT=${PROJECT_SOURCE_DIR}/dependencies/cmake/VerifyManifestProduct.cmake
        -P ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_manifest_product_verifier.cmake
    LABELS dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME dependency-prefix-verifier
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_verify_prefix.sh
    LABELS dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME dependency-prefix-publisher
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_publish_prefix.sh
    LABELS dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME dependency-apple-gpu-vulkan-prefix-verifier
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_verify_apple_gpu_vulkan_prefix.sh
    LABELS dependency gpu
    TIMEOUT 45)

uml_add_tooling_test(
    NAME dependency-runtime-closure
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_verify_runtime_dependencies.sh
    LABELS dependency
    TIMEOUT 30)
