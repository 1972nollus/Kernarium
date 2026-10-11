# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME source-tree-cxx-only
    COMMAND ${CMAKE_COMMAND}
        -DAUDIT_SCRIPT=${PROJECT_SOURCE_DIR}/scripts/audit-source-tree.cmake
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/source-tree-audit-fixture
        -P ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_source_tree_audit.cmake
    LABELS audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME xcframework-output-safety
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_xcframework_destination.sh
        ${PROJECT_SOURCE_DIR}/scripts/build-xcframework.sh
    LABELS audit;package
    TIMEOUT 30)

uml_add_tooling_test(
    NAME git-identity-environment-contract
    COMMAND
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_git_identity_contract.sh
    LABELS audit;kernel;dependency;android
    TIMEOUT 120)

uml_add_tooling_test(
    NAME patch-series-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_patch_series.py
    LABELS audit;kernel;dependency;android
    TIMEOUT 120)

uml_add_tooling_test(
    NAME apple-xcframework-staging
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_create_apple_xcframework.sh
        ${PROJECT_SOURCE_DIR}/scripts/create-apple-xcframework.py
    LABELS audit;package
    TIMEOUT 30)

# The XCFramework, Kernarium framework and Kernarium app packaging run this
# check on every iOS product; the fixtures show it finds FEXCore as a symbol,
# an archive member, a nested framework and a stripped allocation name.
uml_add_tooling_test(
    NAME ios-products-carry-no-fexcore
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_check_no_fexcore.py
        --checker ${PROJECT_SOURCE_DIR}/scripts/check-no-fexcore.py
    LABELS audit;package
    TIMEOUT 120)

uml_add_tooling_test(
    NAME json-input-duplicate-key-policy
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_json_input_policy.py
        --validator
            ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/validate_json_inputs.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME repository-environment-portability
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_environment_portability.py
        --checker
            ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/check_environment_portability.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME engine-kind-link-audit
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_engine_kind_link_audit.py
        --cmake ${CMAKE_COMMAND}
        --audit ${PROJECT_SOURCE_DIR}/scripts/audit-cmake-file-api.cmake
    LABELS audit;cpu
    TIMEOUT 60)
