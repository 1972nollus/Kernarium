# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME dependency-source-preparer
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_prepare.sh
    LABELS dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME dependency-patch-regenerator
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_rebase_patches.sh
    LABELS dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME dependency-build-manifest
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_build_manifest.sh
    LABELS dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME dependency-prefix-verifier
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_verify_prefix.sh
    LABELS dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME dependency-runtime-closure
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_verify_runtime_dependencies.sh
    LABELS dependency
    TIMEOUT 30)
