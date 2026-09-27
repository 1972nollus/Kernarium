# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME kernel-slice-producer-contract
    COMMAND
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_kernel_slice_contract.sh
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-series-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_kernel_series.py
    LABELS kernel;android;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME kernel-artifact-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_kernel_artifact_contract.py
        --artifact-module
            ${PROJECT_SOURCE_DIR}/kernel/common/artifacts/kernel_artifact.py
        --schema
            ${PROJECT_SOURCE_DIR}/kernel/common/artifacts/kernel-artifact-manifest.schema.json
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-git-source-preparer
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_materialize_git.sh
    LABELS kernel;dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-source-location-config
    COMMAND
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_source_location_config.sh
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-darwin-host-compat
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_darwin_host_compat.sh
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME apple-shared-kernel-image-policy
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_apple_kernel_image_policy.py
        --artifact-cmake
            ${PROJECT_SOURCE_DIR}/kernel/common/artifacts/KernelArtifact.cmake
        --carrier-verifier
            ${PROJECT_SOURCE_DIR}/Kernarium/scripts/verify-signed-kernel-carrier.py
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-darwin-irq-edge
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_darwin_irq_edge.py
    LABELS kernel
    TIMEOUT 120)
