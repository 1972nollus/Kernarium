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

uml_add_tooling_test(
    NAME json-input-duplicate-key-policy
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_json_input_policy.py
        --validator
            ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/validate_json_inputs.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS audit
    TIMEOUT 30)

if(UML_ENABLE_RUNTIME_PROFILING)
    add_executable(uml_jit_profile_final_report_fixture
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/jit_profile_final_report_fixture.cpp)
    target_link_libraries(uml_jit_profile_final_report_fixture PRIVATE
        UML::Emulator
        UML::GuestPageConfiguration
        UML::JitProfileConfiguration)
    target_include_directories(uml_jit_profile_final_report_fixture PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_features(uml_jit_profile_final_report_fixture PRIVATE
        cxx_std_20)
    target_compile_options(uml_jit_profile_final_report_fixture PRIVATE
        -Wall -Wextra -Werror)
    uml_add_tooling_test(
        NAME jit-profile-final-report-contract
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_jit_profile_final_report.py
            --fixture $<TARGET_FILE:uml_jit_profile_final_report_fixture>
        LABELS audit;cpu
        TIMEOUT 30)
endif()

uml_add_tooling_test(
    NAME symmetric-engine-edge-audit
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_engine_edge_audit.py
        --cmake ${CMAKE_COMMAND}
        --audit ${PROJECT_SOURCE_DIR}/scripts/audit-cmake-file-api.cmake
        --build-root ${CMAKE_BINARY_DIR}
        --manifest ${CMAKE_BINARY_DIR}/uml-architecture-targets.tsv
    LABELS audit;cpu
    TIMEOUT 60)
