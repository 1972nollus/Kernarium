# SPDX-License-Identifier: GPL-2.0-or-later

find_package(Python3 REQUIRED COMPONENTS Interpreter)

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

uml_add_tooling_test(
    NAME kernel-release-source-preparer
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_materialize_release.sh
    LABELS kernel;dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-series-topology
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_series_topology.sh
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-cmake-build-layout
    COMMAND ${CMAKE_COMMAND}
        -DLAYOUT_MODULE=${PROJECT_SOURCE_DIR}/kernel/CMakeBuildLayout.cmake
        -P ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_cmake_build_layout.cmake
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-compiler-selection
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_kernel_toolchain.sh
        ${PROJECT_SOURCE_DIR}/kernel/KernelToolchain.cmake
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-next-version-policy-actions
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_next_version_policy.sh
    LABELS kernel;audit
    TIMEOUT 30)

# A CLI configuration always supplies the prepared replay. A tooling-only
# configuration may opt in by naming one explicitly; the fixture itself is
# host-only and rootfs-free.
if(DEFINED UML_KERNEL_SOURCE_DIR AND
   EXISTS "${UML_KERNEL_SOURCE_DIR}/arch/um/kernel/tlb.c")
    uml_add_tooling_test(
        NAME darwin-init-mm-tlb-policy
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_darwin_init_mm_tlb_policy.py
            --linux-source ${UML_KERNEL_SOURCE_DIR}
        LABELS kernel;audit
        TIMEOUT 30)
endif()

uml_add_tooling_test(
    NAME abi-declaration-identity
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/abi/test_declaration_identity.sh
    LABELS abi;audit
    TIMEOUT 30)

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

uml_add_tooling_test(
    NAME ltp-runner-parser
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/ltp/run_guest_ltp_tests.py
    LABELS kernel;ltp
    TIMEOUT 120)

uml_add_tooling_test(
    NAME rootfs-provisioning
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_provision.py
    LABELS rootfs;provisioning
    TIMEOUT 120)

uml_add_tooling_test(
    NAME rootfs-maintenance-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_maintenance.py
    LABELS rootfs;maintenance
    TIMEOUT 120)

uml_add_tooling_test(
    NAME rootfs-overlay-launcher-options
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_boot_overlay_systemd.py
        --launcher ${PROJECT_SOURCE_DIR}/scripts/boot-overlay-systemd.sh
    LABELS rootfs;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME gpu-guest-rootfs-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_run_gpu_guest.py
    LABELS gpu
    TIMEOUT 120)

uml_add_tooling_test(
    NAME kde-vnc-launcher-options
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_boot_kde_vnc.py
    LABELS gpu;vnc
    TIMEOUT 120)

uml_add_tooling_test(
    NAME chromium-webgl-aquarium-metrics
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_run_chromium_webgl.py
    LABELS gpu;webgl
    TIMEOUT 120)

uml_add_tooling_test(
    NAME network-smoke-taxonomy
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/network/test_run_guest_network_smoke.py
    LABELS network;slirp
    TIMEOUT 120)

uml_add_tooling_test(
    NAME jit-gpr-link-profile-analysis
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/performance/analyze_jit_gpr_link_profile_tests.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tests/perf
    LABELS cpu;jit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME performance-runner-cli
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/performance/test_cli.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS performance
    TIMEOUT 30)

uml_add_tooling_test(
    NAME arm64-instruction-inventory
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/validate_a64_instruction_inventory.py
    LABELS cpu
    TIMEOUT 120)

execute_process(
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/validate_a64_aslref_corpus.py
    RESULT_VARIABLE _uml_aslref_corpus_identity_result
    OUTPUT_VARIABLE _uml_aslref_corpus_identity_output
    ERROR_VARIABLE _uml_aslref_corpus_identity_error)
if(NOT _uml_aslref_corpus_identity_result EQUAL 0)
    message(FATAL_ERROR
        "Checked-in A64 ASLRef corpus identity is invalid:\n"
        "${_uml_aslref_corpus_identity_output}"
        "${_uml_aslref_corpus_identity_error}")
endif()

uml_add_tooling_test(
    NAME arm64-aslref-corpus-identity
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/validate_a64_aslref_corpus.py
    LABELS cpu;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME arm64-one-shot-publication-safety
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_one_shot_publications.py
    LABELS cpu;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME arm64-instruction-evidence-negative-fixtures
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_instruction_evidence_validator.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-instruction-aggregate-negative-fixtures
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_instruction_aggregate_manifest.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-raw-evidence-compression-fixtures
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_raw_evidence_compression.py
        $<TARGET_FILE:uml_instruction_evidence_test_runner>
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-input-materialization-negative-fixtures
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_input_materialization.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-module-composition
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_module_composition.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-execution-oracle
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_execution_oracle.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-materialization-batch
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_materialization_batch.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-campaign-publication
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_campaign.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-campaign-pilot
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_campaign_pilot.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-campaign-authority
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_campaign_authority.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-campaign-sharding
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_campaign_sharding.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-campaign-result-join
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_campaign_result_join.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-user-state-interface
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_user_state_interface.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-system-state
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_system_state.py
    LABELS cpu;audit
    TIMEOUT 120)

if(UML_BUILD_UNIT_TESTS)
    get_property(_uml_unit_evidence_dependencies GLOBAL PROPERTY UML_UNIT_TESTS)
    uml_add_tooling_test(
        NAME arm64-instruction-evidence
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/validate_a64_instruction_evidence.py
            --evidence-directory ${CMAKE_BINARY_DIR}/instruction-evidence
            --build-root ${CMAKE_BINARY_DIR}
        LABELS cpu;audit;instruction-evidence-validation
        TIMEOUT 120)
    # DEPENDS orders producers that are already selected. Selecting only this
    # validator preserves and checks an existing evidence directory instead
    # of selecting either the reset or any producer implicitly.
    set_tests_properties(arm64-instruction-evidence PROPERTIES
        DEPENDS "${_uml_unit_evidence_dependencies}")

    # Raw inputs remain beside their producer evidence so missing producers are
    # observable. The much larger resumable join state may be placed on a
    # separately provisioned volume without weakening complete-coverage checks.
    set(_uml_input_materialization_storage_args
        --free-space-reserve-gib 5)
    if(UML_INSTRUCTION_MATERIALIZATION_WORK_ROOT)
        set(_uml_input_materialization_profile_root
            "${UML_INSTRUCTION_MATERIALIZATION_WORK_ROOT}/${UML_GUEST_PAGE_SIZE}")
        list(APPEND _uml_input_materialization_storage_args
            --contract-index-output
                "${_uml_input_materialization_profile_root}/a64-input-materialization-contract-index.tsv.gz"
            --checkpoint-directory
                "${_uml_input_materialization_profile_root}/a64-input-materialization-checkpoints")
    endif()

    # Registered only alongside its producers. The sidecars they emit are the
    # dominant build-tree cost, so both sides share one option and the join can
    # never be selected without the inputs it requires.
    if(UML_INSTRUCTION_INPUT_MATERIALIZATION)
        uml_add_tooling_test(
            NAME arm64-input-materialization-join
            COMMAND ${Python3_EXECUTABLE} -B
                ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/join_a64_input_materializations.py
                --evidence-directory ${CMAKE_BINARY_DIR}/instruction-evidence
                --guest-page-size ${UML_GUEST_PAGE_SIZE}
                --output
                    ${CMAKE_BINARY_DIR}/instruction-evidence/a64-input-materialization-index.tsv
                ${_uml_input_materialization_storage_args}
            LABELS cpu;audit;instruction-evidence-validation
            # A complete profile streams roughly 30 million contributions
            # through 449 resumable producer checkpoints. Keep the exhaustive
            # tooling gate bounded, but do not impose the old ten-minute
            # fixture-scale limit.
            TIMEOUT 14400)
        set_tests_properties(arm64-input-materialization-join PROPERTIES
            DEPENDS "arm64-instruction-evidence;${_uml_unit_evidence_dependencies}")
    endif()
endif()

if(UML_BUILD_NETWORK_PROVIDER)
    add_library(uml_network_provider_test_variant STATIC
        ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp/provider.cpp
        ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp/event_loop.cpp
        ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp/frame_queue.cpp)
    target_include_directories(uml_network_provider_test_variant
        PUBLIC
            ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp
            ${PROJECT_SOURCE_DIR}/darwin/include
        PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin/device/network)
    target_compile_definitions(uml_network_provider_test_variant PUBLIC
        UML_SLIRP_TESTING=1)
    target_compile_features(uml_network_provider_test_variant PUBLIC cxx_std_20)
    target_compile_options(uml_network_provider_test_variant PRIVATE
        -Wall -Wextra -Werror)
    target_link_libraries(uml_network_provider_test_variant PRIVATE
        UML::ThirdPartySlirp)
    add_dependencies(uml_network_provider_test_variant
        uml_dependency_prefix_verify)

    add_executable(uml_slirp_network_tests
        ${PROJECT_SOURCE_DIR}/tests/tooling/network/slirp_arp_probe.cpp)
    target_link_libraries(uml_slirp_network_tests PRIVATE
        uml_network_provider_test_variant
        UML::ThirdPartySlirp)
    target_include_directories(uml_slirp_network_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp)
    target_compile_features(uml_slirp_network_tests PRIVATE cxx_std_20)
    target_compile_options(uml_slirp_network_tests PRIVATE
        -Wall -Wextra -Werror)

    uml_add_tooling_test(
        NAME slirp-network-arp
        COMMAND uml_slirp_network_tests
        LABELS network
        TIMEOUT 30)
endif()

if(UML_BUILD_KERNEL_RUNNER)
    add_executable(uml_embedded_kernel_serialization_tests
        ${PROJECT_SOURCE_DIR}/tests/tooling/emulator/embedded_kernel_serialization_tests.cpp)
    target_link_libraries(uml_embedded_kernel_serialization_tests PRIVATE
        uml_emulator_admission_test_variant)
    target_include_directories(uml_embedded_kernel_serialization_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${PROJECT_SOURCE_DIR}/darwin/emulator/src)
    target_compile_features(uml_embedded_kernel_serialization_tests PRIVATE
        cxx_std_20)
    target_compile_options(uml_embedded_kernel_serialization_tests PRIVATE
        -Wall -Wextra -Werror)
    target_compile_definitions(uml_embedded_kernel_serialization_tests PRIVATE
        UML_EMULATOR_ADMISSION_TESTING=1)

    uml_add_tooling_test(
        NAME embedded-kernel-handle-serialization
        COMMAND uml_embedded_kernel_serialization_tests
        LABELS emulator;loader
        TIMEOUT 30)
endif()

if(TARGET uml-virtio-gpu AND TARGET uml-vnc-smoke-client)
    uml_add_tooling_test(
        NAME virtio-gpu-vnc-smoke
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/run_vnc_smoke.py
            --server $<TARGET_FILE:uml-virtio-gpu>
            --client $<TARGET_FILE:uml-vnc-smoke-client>
        LABELS gpu;vnc
        TIMEOUT 30
        PROCESSORS 1
        RESOURCE_GROUPS gpus:1)
endif()
