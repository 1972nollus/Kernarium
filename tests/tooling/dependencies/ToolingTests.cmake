# SPDX-License-Identifier: GPL-2.0-or-later

# Source archive extraction and preparation intentionally exercise the
# Python-based build helper.  Keep them in the host tooling suite rather than
# presenting them as Python-free unit tests.
uml_add_tooling_test(
    NAME dependency-source-archive-verifier
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_source_archive.sh
    LABELS dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME dependency-source-preparer
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_prepare.sh
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
    NAME dependency-production-link-allowlist
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_verify_production_link.sh
    LABELS dependency;audit
    TIMEOUT 30)
