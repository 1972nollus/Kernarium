# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME source-tree-cxx-only
    COMMAND ${CMAKE_COMMAND}
        -DAUDIT_SCRIPT=${PROJECT_SOURCE_DIR}/scripts/audit-source-tree.cmake
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/source-tree-audit-fixture
        -DSOURCE_SIZE_REPORT=${CMAKE_BINARY_DIR}/reports/source-sizes.tsv
        -P ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_source_tree_audit.cmake
    LABELS audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME source-size-advisory-policy
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_SIZE_MODULE=${PROJECT_SOURCE_DIR}/cmake/SourceSizeReport.cmake
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/source-size-policy-fixture
        -P ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_source_size_report.cmake
    LABELS audit
    TIMEOUT 30)

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

set(_uml_jit_profile_preset_command
    ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_jit_profile_preset.py
    --cmake ${CMAKE_COMMAND}
    --source-root ${PROJECT_SOURCE_DIR})
uml_add_tooling_test(
    NAME jit-profile-preset-contract
    COMMAND ${_uml_jit_profile_preset_command}
    LABELS audit;cpu
    TIMEOUT 30)
unset(_uml_jit_profile_preset_command)

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

get_filename_component(_uml_tooling_fixture_parent
    "${CMAKE_BINARY_DIR}" DIRECTORY)
get_filename_component(_uml_tooling_build_name
    "${CMAKE_BINARY_DIR}" NAME)
set(_uml_test_manifest_audit_root
    "${_uml_tooling_fixture_parent}/test-manifest-audit-fixture-${_uml_tooling_build_name}")

uml_add_tooling_test(
    NAME test-manifest-suite-label-policy
    COMMAND ${CMAKE_COMMAND}
        -DAUDIT_SCRIPT=${PROJECT_SOURCE_DIR}/scripts/audit-test-manifest.cmake
        -DTEST_ROOT=${_uml_test_manifest_audit_root}
        -DREGISTRATION_POLICY_MODULE=${PROJECT_SOURCE_DIR}/cmake/TestRegistrationPolicy.cmake
        -DUML_TESTING_MODULE=${PROJECT_SOURCE_DIR}/cmake/UmlTesting.cmake
        -P ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_test_manifest_audit.cmake
    LABELS audit
    TIMEOUT 30)

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
