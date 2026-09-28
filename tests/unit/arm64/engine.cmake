# SPDX-License-Identifier: GPL-2.0-or-later
#
# This family manifest is included by the ARM64 unit-test directory.
    # Compose ordinary tests from the production engine targets. Focused JIT
    # diagnostic tests substitute the counter-enabled executor variant;
    # common conformance stays on the ordinary runtime path.
    set(UML_STATIC_RUNTIME_TEST_BASE_OBJECTS
        $<TARGET_OBJECTS:uml_host_topology>
        $<TARGET_OBJECTS:uml_run_routing>
        $<TARGET_OBJECTS:uml_static_runtime>
        $<TARGET_OBJECTS:uml_cpu_engine_factory>
        $<TARGET_OBJECTS:uml_arm64_gadget_engine>
        $<TARGET_OBJECTS:uml_arm64_jit_engine>
        $<TARGET_OBJECTS:uml_arm64_gadget_executor>
        $<TARGET_OBJECTS:uml_arm64_jit>
        $<TARGET_OBJECTS:uml_arm64_jit_test_support>
        $<TARGET_OBJECTS:uml_static_guest_memory>
        $<TARGET_OBJECTS:uml_runtime_context>)
    if(TARGET uml_x86_64_gadgets)
        list(APPEND UML_STATIC_RUNTIME_TEST_BASE_OBJECTS
            $<TARGET_OBJECTS:uml_x86_64_gadgets>)
        # Static-runtime test binaries compose production OBJECT carriers
        # directly, so carry the x87 archives that would otherwise propagate
        # through the production component target.
        link_libraries(UML::ThirdPartyCephes128)
    endif()
    set(UML_STATIC_RUNTIME_TEST_OBJECTS
        ${UML_STATIC_RUNTIME_TEST_BASE_OBJECTS}
        $<TARGET_OBJECTS:uml_arm64_jit_executor>)
    set(UML_STATIC_RUNTIME_DIAGNOSTIC_TEST_OBJECTS
        ${UML_STATIC_RUNTIME_TEST_BASE_OBJECTS}
        $<TARGET_OBJECTS:uml_arm64_jit_diagnostics_executor>)
    # uml_add_arm64_unit_test() OBJECTS for tests that enter the static
    # runtime, with and without the guest engine selector, and with the
    # diagnostics executor.
    set(UML_ARM64_RUNTIME_TEST_OBJECTS
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    set(UML_ARM64_RUNTIME_DIAGNOSTIC_TEST_OBJECTS
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_DIAGNOSTIC_TEST_OBJECTS})
    set(UML_ARM64_STATIC_TEST_OBJECTS
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})

    # The instruction corpus is zlib-compressed. Reuse the checked private
    # zlib when desktop dependencies are enabled; otherwise import only the
    # same pinned archive without broadening production linkage.
    if(TARGET UML::ThirdPartyZlib)
        set(_uml_arm64_corpus_zlib UML::ThirdPartyZlib)
    else()
        set(_uml_arm64_corpus_zlib_archive
            "${UML_DEPENDENCY_PREFIX}/lib/libz.a")
        set(_uml_arm64_corpus_zlib_include
            "${UML_DEPENDENCY_PREFIX}/include")
        if(NOT EXISTS "${_uml_arm64_corpus_zlib_archive}" OR
           NOT EXISTS "${_uml_arm64_corpus_zlib_include}/zlib.h")
            message(FATAL_ERROR
                "the A64 instruction corpus requires the pinned private zlib")
        endif()
        add_library(uml_arm64_corpus_zlib STATIC IMPORTED)
        set_target_properties(uml_arm64_corpus_zlib PROPERTIES
            IMPORTED_LOCATION "${_uml_arm64_corpus_zlib_archive}"
            INTERFACE_INCLUDE_DIRECTORIES
                "${_uml_arm64_corpus_zlib_include}")
        set(_uml_arm64_corpus_zlib uml_arm64_corpus_zlib)
    endif()

    # Focused tests that need engine hooks, host-private entry state, or
    # engine options run their cases through the production gadget and JIT
    # engines with this harness.
    add_library(uml_arm64_engine_test_support STATIC
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_engine_test_support.cpp)
    target_link_libraries(uml_arm64_engine_test_support PRIVATE
        UML::Emulator)
    target_link_libraries(uml_arm64_engine_test_support PUBLIC UML::BridgeAbi)
    target_include_directories(uml_arm64_engine_test_support PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
    target_compile_features(uml_arm64_engine_test_support PUBLIC cxx_std_20)
    target_compile_options(uml_arm64_engine_test_support PRIVATE
        -Wall -Wextra -Werror)

    # The checked-in A64 instruction corpus runs through the production gadget
    # and JIT engines in one process. tests/unit/arm64/corpus/README.md owns
    # the pack format and its regeneration.
    add_executable(uml_arm64_instruction_corpus_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/instruction_corpus_runner.cpp)
    target_link_libraries(uml_arm64_instruction_corpus_tests PRIVATE
        UML::BridgeAbi
        UML::Emulator
        ${_uml_arm64_corpus_zlib})
    target_include_directories(uml_arm64_instruction_corpus_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_features(uml_arm64_instruction_corpus_tests PRIVATE
        cxx_std_20)
    target_compile_options(uml_arm64_instruction_corpus_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-instruction-corpus
        COMMAND uml_arm64_instruction_corpus_tests
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/corpus/a64_instruction_corpus.pack)

    add_executable(uml_arm64_engine_test_support_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_engine_test_support_tests.cpp)
    target_link_libraries(uml_arm64_engine_test_support_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(uml_arm64_engine_test_support_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-engine-harness
        COMMAND uml_arm64_engine_test_support_tests)

    add_executable(uml_arm64_deferred_fp_state_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_deferred_fp_state_tests.cpp)
    target_link_libraries(uml_arm64_deferred_fp_state_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(uml_arm64_deferred_fp_state_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-deferred-fp-state
        COMMAND uml_arm64_deferred_fp_state_tests)

    add_executable(uml_host_instruction_probe_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/host_instruction_probe_tests.cpp)
    target_include_directories(uml_host_instruction_probe_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_features(uml_host_instruction_probe_tests PRIVATE cxx_std_20)
    target_compile_options(uml_host_instruction_probe_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-host-instruction-probe
        COMMAND uml_host_instruction_probe_tests)

    add_executable(uml_tlb_sync_range_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/tlb_sync_range_tests.cpp)
    target_link_libraries(uml_tlb_sync_range_tests PRIVATE UML::BridgeAbi)
    target_compile_features(uml_tlb_sync_range_tests PRIVATE cxx_std_20)
    target_compile_options(uml_tlb_sync_range_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME uml-tlb-sync-ranges COMMAND uml_tlb_sync_range_tests)

    add_executable(uml_arm64_gadget_tlb_layout_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_tlb_layout_tests.cpp)
    target_include_directories(uml_arm64_gadget_tlb_layout_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_features(uml_arm64_gadget_tlb_layout_tests PRIVATE cxx_std_20)
    target_compile_options(uml_arm64_gadget_tlb_layout_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-tlb-layout
        COMMAND uml_arm64_gadget_tlb_layout_tests)

	add_executable(uml_cpu_engine_contract_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/cpu_engine_contract_tests.cpp)
	target_link_libraries(uml_cpu_engine_contract_tests PRIVATE
		UML::BridgeAbi
		UML::Emulator)
	target_include_directories(uml_cpu_engine_contract_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/emulator)
	target_compile_features(uml_cpu_engine_contract_tests PRIVATE cxx_std_20)
	target_compile_options(uml_cpu_engine_contract_tests PRIVATE
		-Wall -Wextra -Werror)
	uml_add_unit_test(NAME cpu-engine-contract COMMAND uml_cpu_engine_contract_tests)

	add_executable(uml_arm64_pc_alignment_contract_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_pc_alignment_contract_tests.cpp)
	target_link_libraries(uml_arm64_pc_alignment_contract_tests PRIVATE
		UML::BridgeAbi
		UML::Emulator)
	target_include_directories(uml_arm64_pc_alignment_contract_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/tests/unit/arm64)
	target_compile_features(uml_arm64_pc_alignment_contract_tests PRIVATE
		cxx_std_20)
	target_compile_options(uml_arm64_pc_alignment_contract_tests PRIVATE
		-Wall -Wextra -Werror)
	uml_add_unit_test(NAME arm64-pc-alignment-contract
		COMMAND uml_arm64_pc_alignment_contract_tests
		LABELS tsan)

    add_executable(uml_arm64_floating_exception_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_floating_exception_tests.cpp)
    target_include_directories(uml_arm64_floating_exception_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_options(uml_arm64_floating_exception_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-floating-exception
        COMMAND uml_arm64_floating_exception_tests)

    uml_add_arm64_unit_test(floating_trap_control)

    uml_add_arm64_unit_test(floating_trap_runtime WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(gadget_terminal_count WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_STATIC_TEST_OBJECTS})

    uml_add_arm64_unit_test(gadget_successor_hoist_structure WITHOUT_HARNESS)
    uml_add_arm64_unit_test(static_runtime WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_STATIC_TEST_OBJECTS} LABELS tsan)
    uml_add_arm64_unit_test(static_boundary_state WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_STATIC_TEST_OBJECTS})
    uml_add_arm64_unit_test(static_translation WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_STATIC_TEST_OBJECTS})
    uml_add_arm64_unit_test(tlb WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_STATIC_TEST_OBJECTS})

    add_executable(uml_arm64_jit_cache_geometry_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_cache_geometry_tests.cpp)
    target_link_libraries(uml_arm64_jit_cache_geometry_tests PRIVATE
        UML::BridgeAbi
        uml_cpu_jit_component)
    target_include_directories(uml_arm64_jit_cache_geometry_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_features(uml_arm64_jit_cache_geometry_tests PRIVATE
        cxx_std_20)
    target_compile_options(uml_arm64_jit_cache_geometry_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-cache-geometry
        COMMAND uml_arm64_jit_cache_geometry_tests
        LABELS tsan)

    add_executable(uml_arm64_jit_shared_translation_cache_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_shared_translation_cache_tests.cpp)
    target_link_libraries(uml_arm64_jit_shared_translation_cache_tests PRIVATE
        UML::BridgeAbi
        uml_cpu_jit_component
        uml_test_allocation_failure)
    target_include_directories(
        uml_arm64_jit_shared_translation_cache_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_features(uml_arm64_jit_shared_translation_cache_tests
        PRIVATE cxx_std_20)
    target_compile_options(uml_arm64_jit_shared_translation_cache_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-shared-translation-cache
        COMMAND uml_arm64_jit_shared_translation_cache_tests
        LABELS tsan)

    add_executable(uml_arm64_jit_executor_pool_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_executor_pool_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_DIAGNOSTIC_TEST_OBJECTS})
    target_link_libraries(uml_arm64_jit_executor_pool_tests PRIVATE
        UML::BridgeAbi)
    target_include_directories(uml_arm64_jit_executor_pool_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
    target_compile_features(uml_arm64_jit_executor_pool_tests PRIVATE
        cxx_std_20)
    target_compile_definitions(uml_arm64_jit_executor_pool_tests PRIVATE
        UML_JIT_RUNTIME_DIAGNOSTICS=1)
    target_compile_options(uml_arm64_jit_executor_pool_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-executor-pool
        COMMAND uml_arm64_jit_executor_pool_tests
        LABELS tsan)

    add_executable(uml_arm64_single_step_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_single_step_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_link_libraries(uml_arm64_single_step_tests PRIVATE UML::BridgeAbi)
    target_include_directories(uml_arm64_single_step_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
    target_compile_features(uml_arm64_single_step_tests PRIVATE cxx_std_20)
    target_compile_options(uml_arm64_single_step_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-single-step
        COMMAND uml_arm64_single_step_tests)

    add_executable(uml_arm64_jit_executor_pool_multislot_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_executor_pool_multislot_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_DIAGNOSTIC_TEST_OBJECTS})
    target_link_libraries(uml_arm64_jit_executor_pool_multislot_tests PRIVATE
        UML::BridgeAbi)
    target_include_directories(
        uml_arm64_jit_executor_pool_multislot_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
    target_compile_features(uml_arm64_jit_executor_pool_multislot_tests
        PRIVATE cxx_std_20)
    target_compile_definitions(uml_arm64_jit_executor_pool_multislot_tests
        PRIVATE UML_JIT_RUNTIME_DIAGNOSTICS=1)
    target_compile_options(uml_arm64_jit_executor_pool_multislot_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-executor-pool-multislot
        COMMAND uml_arm64_jit_executor_pool_multislot_tests
        LABELS tsan)

    foreach(JIT_TEST IN ITEMS
            runtime
            capability
            cache_validation
            barrier_codegen
            barrier_runtime
            illegal_runtime
            publication_gate
            reservation_alias)
        set(_jit_test_runtime_objects ${UML_STATIC_RUNTIME_TEST_OBJECTS})
        if(JIT_TEST STREQUAL "cache_validation")
            set(_jit_test_runtime_objects
                ${UML_STATIC_RUNTIME_DIAGNOSTIC_TEST_OBJECTS})
        endif()
        add_executable(uml_arm64_jit_${JIT_TEST}_tests
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_${JIT_TEST}_tests.cpp
            $<TARGET_OBJECTS:uml_runtime_guest_selector>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_arm64_execution_helpers>
            $<TARGET_OBJECTS:uml_static_mmu>
            ${_jit_test_runtime_objects})
        target_link_libraries(uml_arm64_jit_${JIT_TEST}_tests PRIVATE
            UML::BridgeAbi)
        target_include_directories(uml_arm64_jit_${JIT_TEST}_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin)
        target_compile_options(uml_arm64_jit_${JIT_TEST}_tests PRIVATE
            -Wall -Wextra -Werror)
        if(JIT_TEST STREQUAL "cache_validation")
            target_compile_definitions(
                uml_arm64_jit_${JIT_TEST}_tests PRIVATE
                UML_JIT_RUNTIME_DIAGNOSTICS=1)
            target_link_libraries(
                uml_arm64_jit_${JIT_TEST}_tests PRIVATE
                uml_test_allocation_failure)
        endif()
        uml_add_unit_test(NAME arm64-jit-${JIT_TEST}
            COMMAND uml_arm64_jit_${JIT_TEST}_tests)
    endforeach()
    set_property(TEST arm64-jit-publication_gate APPEND PROPERTY LABELS tsan)

    uml_add_arm64_unit_test(jit_lazy_flags_runtime WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_lazy_flags_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    foreach(JIT_DIAGNOSED_TEST IN ITEMS indirect_link chain_abi)
        uml_add_arm64_unit_test(jit_${JIT_DIAGNOSED_TEST}_codegen WITHOUT_HARNESS
            OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
        uml_add_arm64_unit_test(jit_${JIT_DIAGNOSED_TEST}_runtime WITHOUT_HARNESS
            OBJECTS ${UML_ARM64_RUNTIME_DIAGNOSTIC_TEST_OBJECTS})
        target_compile_definitions(
            uml_arm64_jit_${JIT_DIAGNOSED_TEST}_runtime_tests PRIVATE
            UML_JIT_RUNTIME_DIAGNOSTICS=1)
    endforeach()
    target_sources(uml_arm64_jit_chain_abi_runtime_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_chain_abi_probe.S)
    foreach(JIT_EXACT_RETIREMENT_TEST IN ITEMS codegen runtime)
        uml_add_arm64_unit_test(
            jit_exact_retirement_${JIT_EXACT_RETIREMENT_TEST} WITHOUT_HARNESS
            OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    endforeach()

    uml_add_arm64_unit_test(jit_fp_link_runtime WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    uml_add_arm64_unit_test(counter_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    uml_add_arm64_unit_test(jit_counter_runtime WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    uml_add_arm64_unit_test(jit_register_remap_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    foreach(JIT_ONE_TO_ONE_TEST IN ITEMS base fp_simd)
        uml_add_arm64_unit_test(
            jit_one_to_one_${JIT_ONE_TO_ONE_TEST}_codegen WITHOUT_HARNESS
            NAME arm64-jit-one-to-one-${JIT_ONE_TO_ONE_TEST}-codegen
            OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    endforeach()

    add_executable(uml_arm64_jit_tlb_miss_preservation_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_tlb_miss_preservation_tests.cpp
        ${UML_STATIC_RUNTIME_TEST_OBJECTS}
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_link_libraries(
        uml_arm64_jit_tlb_miss_preservation_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_tlb_miss_preservation_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-tlb-miss-preservation
        COMMAND uml_arm64_jit_tlb_miss_preservation_tests)

    uml_add_arm64_unit_test(jit_sp_memory_run_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    add_executable(uml_arm64_jit_sp_memory_run_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_sp_memory_run_runtime_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_sp_memory_run_runtime_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_sp_memory_run_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-sp-memory-run-runtime
        COMMAND uml_arm64_jit_sp_memory_run_runtime_tests)

    uml_add_arm64_unit_test(jit_gpr_memory_run_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    add_executable(uml_arm64_jit_gpr_memory_run_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_gpr_memory_run_runtime_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_gpr_memory_run_runtime_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_gpr_memory_run_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-gpr-memory-run-runtime
        COMMAND uml_arm64_jit_gpr_memory_run_runtime_tests)

    uml_add_arm64_unit_test(jit_simd_memory_run_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    add_executable(uml_arm64_jit_simd_memory_run_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_simd_memory_run_runtime_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_simd_memory_run_runtime_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_simd_memory_run_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-simd-memory-run-runtime
        COMMAND uml_arm64_jit_simd_memory_run_runtime_tests)

    uml_add_arm64_unit_test(jit_scalar_atomic_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_simd_structure_remap_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_vector_remap_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS}
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/arm64_jit_vector_remap_pressure_tests.cpp)
    uml_add_arm64_unit_test(jit_table_list_remap_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_pair_remap_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_control_remap_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(gadget_runtime WITHOUT_HARNESS)

    add_executable(uml_arm64_gadget_cache_validation_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_cache_validation_tests.cpp
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_link_libraries(uml_arm64_gadget_cache_validation_tests PRIVATE
        UML::BridgeAbi)
    target_include_directories(
        uml_arm64_gadget_cache_validation_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
    target_compile_definitions(
        uml_arm64_gadget_cache_validation_tests PRIVATE
        UML_GADGET_CACHE_SLOTS=${UML_GADGET_CACHE_SLOTS}
        UML_GADGET_RESIDENT_GENERATIONS=${UML_GADGET_RESIDENT_GENERATIONS})
    target_compile_options(
        uml_arm64_gadget_cache_validation_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-cache-validation
        COMMAND uml_arm64_gadget_cache_validation_tests)

    add_executable(uml_arm64_gadget_inline_fallthrough_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_inline_fallthrough_tests.cpp
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_link_libraries(uml_arm64_gadget_inline_fallthrough_tests PRIVATE
        UML::BridgeAbi)
    target_include_directories(
        uml_arm64_gadget_inline_fallthrough_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
    target_compile_options(
        uml_arm64_gadget_inline_fallthrough_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-inline-fallthrough
        COMMAND uml_arm64_gadget_inline_fallthrough_tests)

    uml_add_arm64_unit_test(gadget_hot_path WITHOUT_HARNESS)
    uml_add_arm64_unit_test(gadget_nzcv_residency WITHOUT_HARNESS)
    uml_add_arm64_unit_test(gadget_fp_environment WITHOUT_HARNESS)

    add_executable(uml_arm64_gadget_dependency_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_dependency_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_dependency_reference.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_dependency_memory_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_vector_dependency_tests.cpp
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_link_libraries(uml_arm64_gadget_dependency_tests PRIVATE
        UML::BridgeAbi
        UML::CpuContract uml_arm64_engine_test_support)
    target_include_directories(uml_arm64_gadget_dependency_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_options(uml_arm64_gadget_dependency_tests PRIVATE -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-dependencies COMMAND uml_arm64_gadget_dependency_tests)

    # The test's exit hold must be registered before any destructor it
    # outlasts: keep engine state lazy or in libraries linked after it.
    uml_add_arm64_unit_test(gadget_exit WITHOUT_HARNESS)
    uml_add_arm64_unit_test(gadget_link NAME arm64-gadget-links WITHOUT_HARNESS)
    uml_add_arm64_unit_test(gadget_indirect_link NAME arm64-gadget-indirect-links
        WITHOUT_HARNESS)

    add_executable(uml_gadget_record_table_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/gadget_record_table_tests.cpp)
    target_include_directories(uml_gadget_record_table_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin)
    target_compile_options(uml_gadget_record_table_tests PRIVATE
		-Wall -Wextra -Werror)
    uml_add_unit_test(NAME gadget-record-table
		COMMAND uml_gadget_record_table_tests)
