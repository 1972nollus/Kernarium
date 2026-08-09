# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME source-tree-cxx-only
    COMMAND ${CMAKE_COMMAND}
        -DAUDIT_SCRIPT=${PROJECT_SOURCE_DIR}/scripts/audit-source-tree.cmake
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
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
    NAME apple-static-archive-metadata
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_apple_static_archive_verifier.sh
        ${PROJECT_SOURCE_DIR}/scripts/verify-apple-static-archive.py
    LABELS audit;package
    TIMEOUT 30)

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

find_program(UML_ENGINE_NM_TOOL NAMES llvm-nm nm REQUIRED)
find_program(UML_ENGINE_DISASSEMBLER_TOOL NAMES otool REQUIRED)
set(_uml_engine_object_audit_command
    ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_engine_object_boundaries.py
    --nm ${UML_ENGINE_NM_TOOL}
    --disassembler ${UML_ENGINE_DISASSEMBLER_TOOL}
    --jit-objects $<JOIN:$<TARGET_OBJECTS:uml_arm64_jit>,|>
    --executor-objects $<JOIN:$<TARGET_OBJECTS:uml_arm64_jit_executor>,|>
    --gadget-objects $<JOIN:$<TARGET_OBJECTS:uml_arm64_gadgets>,|>
    --neutral-objects $<JOIN:$<TARGET_OBJECTS:uml_arm64_execution_helpers>,|>
    --adapter-objects $<JOIN:$<TARGET_OBJECTS:uml_cpu_engine_factory>,|>
    --adapter-objects $<JOIN:$<TARGET_OBJECTS:uml_arm64_interpreter_engine>,|>
    --adapter-objects $<JOIN:$<TARGET_OBJECTS:uml_arm64_gadget_engine>,|>
    --adapter-objects $<JOIN:$<TARGET_OBJECTS:uml_arm64_jit_engine>,|>
    --production-archive $<TARGET_FILE:uml_emulator>
    --guest-page-size ${UML_GUEST_PAGE_SIZE}
    --source-root ${PROJECT_SOURCE_DIR})
set(_uml_production_object_targets
    uml_emulator_boundary
    uml_network_device
    uml_run_routing
    uml_runtime_context
    uml_runtime_guest_selector
    uml_runtime_entry
    uml_static_runtime
    uml_cpu_engine_factory
    uml_arm64_dispatch
    uml_arm64_interpreter_engine
    uml_arm64_gadget_engine
    uml_arm64_gadget_executor
    uml_arm64_jit_engine
    uml_arm64_jit
    uml_arm64_jit_executor
    uml_arm64_gadgets
    uml_arm64_execution_helpers
    uml_static_mmu
    uml_static_guest_memory
    uml_memory_vm_bridge)
if(TARGET uml_native_mmu)
    list(APPEND _uml_production_object_targets uml_native_mmu)
endif()
foreach(_uml_production_object_target IN LISTS
        _uml_production_object_targets)
    list(APPEND _uml_engine_object_audit_command
        --configured-production-objects
        $<JOIN:$<TARGET_OBJECTS:${_uml_production_object_target}>,|>)
endforeach()
if(TARGET uml_network_provider)
    list(APPEND _uml_engine_object_audit_command
        --ordinary-product $<TARGET_FILE:uml_network_provider>)
endif()
if(TARGET uml_gpu_transport)
    list(APPEND _uml_engine_object_audit_command
        --ordinary-product $<TARGET_FILE:uml_gpu_transport>)
endif()
if(UML_ENABLE_JIT_PROFILE)
    list(APPEND _uml_engine_object_audit_command --allow-profile)
endif()
if(TARGET uml_arm64_engine_test_support_tests)
    list(APPEND _uml_engine_object_audit_command
        --test-hook-objects
        $<JOIN:$<TARGET_OBJECTS:uml_arm64_jit_test_support>,|>
        --conformance-binary
        $<TARGET_FILE:uml_arm64_engine_test_support_tests>)
endif()
if(TARGET uml_arm64_jit_diagnostics_executor)
    list(APPEND _uml_engine_object_audit_command
        --diagnostic-executor-objects
        $<JOIN:$<TARGET_OBJECTS:uml_arm64_jit_diagnostics_executor>,|>)
endif()
uml_add_tooling_test(
    NAME engine-object-boundary-audit
    COMMAND ${_uml_engine_object_audit_command}
    LABELS audit;cpu
    TIMEOUT 30)

# Record this build's gadget entry addresses and 64-byte cache-line phases.
# The snapshot is what a later optimization trial compares against with
# --baseline, so an accepted build always leaves behind the evidence needed to
# prove that a gadget-body change did not move unrelated entries.
#
# Prefer the linked image. Since G99 turned on interprocedural optimization the
# gadget objects are bitcode, so reading them shows the 9,576 entries defined in
# assembly and none of the 87 defined in C++ - and G101 caught exactly those 87
# moving 752 bytes and changing cache-line phase while every assembly entry
# stayed put. A configuration that does not build the emulator still gets the
# partial snapshot, which is better than none.
if(TARGET uml-darwin)
    set(_uml_gadget_layout_source --image $<TARGET_FILE:uml-darwin>)
else()
    set(_uml_gadget_layout_source
        --objects $<JOIN:$<TARGET_OBJECTS:uml_arm64_gadgets>,|>)
endif()

uml_add_tooling_test(
    NAME gadget-bank-layout-snapshot
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_gadget_bank_layout.py
        --nm ${UML_ENGINE_NM_TOOL}
        ${_uml_gadget_layout_source}
        --write-baseline
            ${CMAKE_BINARY_DIR}/reports/gadget-bank-layout.tsv
    LABELS audit;cpu
    TIMEOUT 60)
