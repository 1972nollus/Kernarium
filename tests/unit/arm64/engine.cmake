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
    target_include_directories(uml_arm64_engine_test_support PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_features(uml_arm64_engine_test_support PUBLIC cxx_std_20)
    target_compile_options(uml_arm64_engine_test_support PRIVATE
        -Wall -Wextra -Werror)

    # The checked-in A64 instruction corpus runs through the production gadget
    # and JIT engines in one process. tests/unit/arm64/corpus/README.md owns
    # the pack format and its regeneration.
    add_executable(uml_arm64_instruction_corpus_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/instruction_corpus_runner.cpp)
    target_link_libraries(uml_arm64_instruction_corpus_tests PRIVATE
        UML::Emulator
        ${_uml_arm64_corpus_zlib})
    target_include_directories(uml_arm64_instruction_corpus_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
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
    target_include_directories(uml_tlb_sync_range_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include)
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

    add_executable(uml_static_mmu_batch_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/static_mmu_batch_tests.cpp
        $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
    target_include_directories(uml_static_mmu_batch_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
    target_compile_features(uml_static_mmu_batch_tests PRIVATE cxx_std_20)
    target_compile_definitions(uml_static_mmu_batch_tests PRIVATE
        UML_MEMORY_ALLOCATION_TESTING=1)
    target_compile_options(uml_static_mmu_batch_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME static-mmu-batch COMMAND uml_static_mmu_batch_tests
        LABELS tsan)

    add_executable(uml_emulator_api_tests ${PROJECT_SOURCE_DIR}/tests/unit/arm64/emulator_api_tests.cpp)
    target_link_libraries(uml_emulator_api_tests PRIVATE
        UML::Emulator
        uml_test_allocation_failure)
    target_include_directories(uml_emulator_api_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_features(uml_emulator_api_tests PRIVATE cxx_std_20)
    target_compile_definitions(uml_emulator_api_tests PRIVATE
        UML_EMULATOR_TEST_HAS_SLIRP=$<BOOL:${UML_BUILD_NETWORK_PROVIDER}>)
    if(TARGET uml_network_provider)
        target_link_libraries(uml_emulator_api_tests PRIVATE
            UML::NetworkProvider)
    endif()
    target_compile_options(uml_emulator_api_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME emulator-native-entry-api COMMAND uml_emulator_api_tests)

	add_executable(uml_emulator_options_tests
		${PROJECT_SOURCE_DIR}/tests/unit/emulator_options_tests.cpp)
	target_link_libraries(uml_emulator_options_tests PRIVATE UML::Emulator)
	target_include_directories(uml_emulator_options_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/emulator/src
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_features(uml_emulator_options_tests PRIVATE cxx_std_20)
	target_compile_options(uml_emulator_options_tests PRIVATE
		-Wall -Wextra -Werror)
	uml_add_unit_test(NAME emulator-options COMMAND uml_emulator_options_tests)

	add_executable(uml_cpu_engine_contract_tests
		${PROJECT_SOURCE_DIR}/tests/unit/cpu_engine_contract_tests.cpp)
	target_link_libraries(uml_cpu_engine_contract_tests PRIVATE UML::Emulator)
	target_include_directories(uml_cpu_engine_contract_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/emulator
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_features(uml_cpu_engine_contract_tests PRIVATE cxx_std_20)
	target_compile_options(uml_cpu_engine_contract_tests PRIVATE
		-Wall -Wextra -Werror)
	uml_add_unit_test(NAME cpu-engine-contract COMMAND uml_cpu_engine_contract_tests)

	add_executable(uml_arm64_pc_alignment_contract_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_pc_alignment_contract_tests.cpp)
	target_link_libraries(uml_arm64_pc_alignment_contract_tests PRIVATE
		UML::Emulator)
	target_include_directories(uml_arm64_pc_alignment_contract_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include
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

    foreach(floating_trap_suite IN ITEMS control)
        add_executable(uml_arm64_floating_trap_${floating_trap_suite}_tests
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_floating_trap_${floating_trap_suite}_tests.cpp
            $<TARGET_OBJECTS:uml_static_mmu>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
        target_include_directories(
            uml_arm64_floating_trap_${floating_trap_suite}_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_floating_trap_${floating_trap_suite}_tests PRIVATE
            -Wall -Wextra -Werror)
        target_link_libraries(
            uml_arm64_floating_trap_${floating_trap_suite}_tests PRIVATE
            uml_arm64_engine_test_support)
        uml_add_unit_test(NAME arm64-floating-trap-${floating_trap_suite}
            COMMAND
                uml_arm64_floating_trap_${floating_trap_suite}_tests)
    endforeach()

    add_executable(uml_arm64_floating_trap_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_floating_trap_runtime_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(uml_arm64_floating_trap_runtime_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_floating_trap_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-floating-trap-runtime
        COMMAND uml_arm64_floating_trap_runtime_tests)

    add_executable(uml_arm64_gadget_terminal_count_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_terminal_count_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(uml_arm64_gadget_terminal_count_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_terminal_count_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-terminal-count
        COMMAND uml_arm64_gadget_terminal_count_tests)

    add_executable(uml_arm64_gadget_successor_hoist_structure_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_successor_hoist_structure_tests.cpp
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(
        uml_arm64_gadget_successor_hoist_structure_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_gadget_successor_hoist_structure_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-successor-hoist-structure
        COMMAND uml_arm64_gadget_successor_hoist_structure_tests)

    add_executable(uml_arm64_static_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_static_runtime_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(uml_arm64_static_runtime_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_link_libraries(uml_arm64_static_runtime_tests PRIVATE
        UML::MemorySharedBacking)
    target_compile_options(uml_arm64_static_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-static-runtime COMMAND uml_arm64_static_runtime_tests
        LABELS tsan)

    add_executable(uml_arm64_static_boundary_state_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_static_boundary_state_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(uml_arm64_static_boundary_state_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_static_boundary_state_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-static-boundary-state
        COMMAND uml_arm64_static_boundary_state_tests)

    add_executable(uml_arm64_static_translation_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_static_translation_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(uml_arm64_static_translation_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_static_translation_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-static-translation
        COMMAND uml_arm64_static_translation_tests)

    add_executable(uml_arm64_tlb_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_tlb_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(uml_arm64_tlb_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_tlb_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-tlb COMMAND uml_arm64_tlb_tests)

    add_executable(uml_arm64_jit_cache_geometry_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_cache_geometry_tests.cpp)
    target_link_libraries(uml_arm64_jit_cache_geometry_tests PRIVATE
        uml_cpu_jit_component)
    target_include_directories(uml_arm64_jit_cache_geometry_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
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
        uml_cpu_jit_component
        uml_test_allocation_failure)
    target_include_directories(
        uml_arm64_jit_shared_translation_cache_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
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
    target_include_directories(uml_arm64_jit_executor_pool_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include
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
    target_include_directories(uml_arm64_single_step_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include
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
    target_include_directories(
        uml_arm64_jit_executor_pool_multislot_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include
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
        target_include_directories(uml_arm64_jit_${JIT_TEST}_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin
            ${PROJECT_SOURCE_DIR}/darwin/include)
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

    add_executable(uml_arm64_jit_lazy_flags_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_lazy_flags_runtime_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(
        uml_arm64_jit_lazy_flags_runtime_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_lazy_flags_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-lazy-flags-runtime
        COMMAND uml_arm64_jit_lazy_flags_runtime_tests)

    add_executable(uml_arm64_jit_lazy_flags_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_lazy_flags_codegen_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(
        uml_arm64_jit_lazy_flags_codegen_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_lazy_flags_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-lazy-flags-codegen
        COMMAND uml_arm64_jit_lazy_flags_codegen_tests)

    foreach(JIT_INDIRECT_LINK_TEST IN ITEMS codegen runtime)
        set(_jit_indirect_runtime_objects ${UML_STATIC_RUNTIME_TEST_OBJECTS})
        if(JIT_INDIRECT_LINK_TEST STREQUAL "runtime")
            set(_jit_indirect_runtime_objects
                ${UML_STATIC_RUNTIME_DIAGNOSTIC_TEST_OBJECTS})
        endif()
        add_executable(
            uml_arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests.cpp
            $<TARGET_OBJECTS:uml_runtime_guest_selector>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_arm64_execution_helpers>
            $<TARGET_OBJECTS:uml_static_mmu>
            ${_jit_indirect_runtime_objects})
        target_include_directories(
            uml_arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests
            PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests
            PRIVATE -Wall -Wextra -Werror)
        if(JIT_INDIRECT_LINK_TEST STREQUAL "runtime")
            target_compile_definitions(
                uml_arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests
                PRIVATE UML_JIT_RUNTIME_DIAGNOSTICS=1)
        endif()
        uml_add_unit_test(
            NAME arm64-jit-indirect-link-${JIT_INDIRECT_LINK_TEST}
            COMMAND
                uml_arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests)
    endforeach()

    foreach(JIT_CHAIN_ABI_TEST IN ITEMS codegen runtime)
        set(_uml_jit_chain_abi_objects ${UML_STATIC_RUNTIME_TEST_OBJECTS})
        if(JIT_CHAIN_ABI_TEST STREQUAL "runtime")
            set(_uml_jit_chain_abi_objects
                ${UML_STATIC_RUNTIME_DIAGNOSTIC_TEST_OBJECTS})
        endif()
        add_executable(
            uml_arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests.cpp
            $<TARGET_OBJECTS:uml_runtime_guest_selector>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_arm64_execution_helpers>
            $<TARGET_OBJECTS:uml_static_mmu>
            ${_uml_jit_chain_abi_objects})
        if(JIT_CHAIN_ABI_TEST STREQUAL "runtime")
            target_sources(
                uml_arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests PRIVATE
                ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_chain_abi_probe.S)
            target_compile_definitions(
                uml_arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests PRIVATE
                UML_JIT_RUNTIME_DIAGNOSTICS=1)
        endif()
        target_include_directories(
            uml_arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests PRIVATE
            -Wall -Wextra -Werror)
        uml_add_unit_test(
            NAME arm64-jit-chain-abi-${JIT_CHAIN_ABI_TEST}
            COMMAND uml_arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests)
        unset(_uml_jit_chain_abi_objects)
    endforeach()

    foreach(JIT_EXACT_RETIREMENT_TEST IN ITEMS codegen runtime)
        add_executable(
            uml_arm64_jit_exact_retirement_${JIT_EXACT_RETIREMENT_TEST}_tests
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_exact_retirement_${JIT_EXACT_RETIREMENT_TEST}_tests.cpp
            $<TARGET_OBJECTS:uml_runtime_guest_selector>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_arm64_execution_helpers>
            $<TARGET_OBJECTS:uml_static_mmu>
            ${UML_STATIC_RUNTIME_TEST_OBJECTS})
        target_include_directories(
            uml_arm64_jit_exact_retirement_${JIT_EXACT_RETIREMENT_TEST}_tests
            PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_jit_exact_retirement_${JIT_EXACT_RETIREMENT_TEST}_tests
            PRIVATE -Wall -Wextra -Werror)
        uml_add_unit_test(
            NAME arm64-jit-exact-retirement-${JIT_EXACT_RETIREMENT_TEST}
            COMMAND
                uml_arm64_jit_exact_retirement_${JIT_EXACT_RETIREMENT_TEST}_tests)
    endforeach()

    add_executable(uml_arm64_jit_fp_link_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_fp_link_runtime_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(
        uml_arm64_jit_fp_link_runtime_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_fp_link_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-fp-link-runtime
        COMMAND uml_arm64_jit_fp_link_runtime_tests)

    add_executable(uml_arm64_counter_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_counter_codegen_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(uml_arm64_counter_codegen_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_counter_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-counter-codegen
        COMMAND uml_arm64_counter_codegen_tests)

    add_executable(uml_arm64_jit_counter_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_counter_runtime_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(uml_arm64_jit_counter_runtime_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_jit_counter_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-counter-runtime
        COMMAND uml_arm64_jit_counter_runtime_tests)

    add_executable(uml_arm64_jit_register_remap_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_register_remap_codegen_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(
        uml_arm64_jit_register_remap_codegen_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_register_remap_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-register-remap-codegen
        COMMAND uml_arm64_jit_register_remap_codegen_tests)

    foreach(JIT_ONE_TO_ONE_TEST IN ITEMS base fp_simd)
        add_executable(
            uml_arm64_jit_one_to_one_${JIT_ONE_TO_ONE_TEST}_codegen_tests
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_one_to_one_${JIT_ONE_TO_ONE_TEST}_codegen_tests.cpp
            $<TARGET_OBJECTS:uml_runtime_guest_selector>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_arm64_execution_helpers>
            $<TARGET_OBJECTS:uml_static_mmu>
            ${UML_STATIC_RUNTIME_TEST_OBJECTS})
        target_include_directories(
            uml_arm64_jit_one_to_one_${JIT_ONE_TO_ONE_TEST}_codegen_tests
            PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_jit_one_to_one_${JIT_ONE_TO_ONE_TEST}_codegen_tests
            PRIVATE
            -Wall -Wextra -Werror)
        uml_add_unit_test(
            NAME arm64-jit-one-to-one-${JIT_ONE_TO_ONE_TEST}-codegen
            COMMAND
                uml_arm64_jit_one_to_one_${JIT_ONE_TO_ONE_TEST}_codegen_tests)
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

    add_executable(uml_arm64_jit_sp_memory_run_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_sp_memory_run_codegen_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(
        uml_arm64_jit_sp_memory_run_codegen_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_sp_memory_run_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-sp-memory-run-codegen
        COMMAND uml_arm64_jit_sp_memory_run_codegen_tests)

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

    add_executable(uml_arm64_jit_gpr_memory_run_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_gpr_memory_run_codegen_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(
        uml_arm64_jit_gpr_memory_run_codegen_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_gpr_memory_run_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-gpr-memory-run-codegen
        COMMAND uml_arm64_jit_gpr_memory_run_codegen_tests)

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

    add_executable(uml_arm64_jit_simd_memory_run_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_simd_memory_run_codegen_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(
        uml_arm64_jit_simd_memory_run_codegen_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_simd_memory_run_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-simd-memory-run-codegen
        COMMAND uml_arm64_jit_simd_memory_run_codegen_tests)

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

    add_executable(uml_arm64_jit_scalar_atomic_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_scalar_atomic_codegen_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(
        uml_arm64_jit_scalar_atomic_codegen_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_scalar_atomic_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-scalar-atomic-codegen
        COMMAND uml_arm64_jit_scalar_atomic_codegen_tests)

    add_executable(uml_arm64_jit_simd_structure_remap_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_simd_structure_remap_codegen_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(
        uml_arm64_jit_simd_structure_remap_codegen_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_simd_structure_remap_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-simd-structure-remap-codegen
        COMMAND uml_arm64_jit_simd_structure_remap_codegen_tests)

    add_executable(uml_arm64_jit_vector_remap_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_vector_remap_codegen_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_vector_remap_pressure_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(
        uml_arm64_jit_vector_remap_codegen_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_vector_remap_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-vector-remap-codegen
        COMMAND uml_arm64_jit_vector_remap_codegen_tests)

    add_executable(uml_arm64_jit_table_list_remap_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_table_list_remap_codegen_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(
        uml_arm64_jit_table_list_remap_codegen_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_table_list_remap_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-table-list-remap-codegen
        COMMAND uml_arm64_jit_table_list_remap_codegen_tests)

    add_executable(uml_arm64_jit_gpr_residency_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_gpr_residency_codegen_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(
        uml_arm64_jit_gpr_residency_codegen_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_gpr_residency_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-gpr-residency-codegen
        COMMAND uml_arm64_jit_gpr_residency_codegen_tests)

    add_executable(uml_arm64_jit_pair_remap_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_pair_remap_codegen_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(
        uml_arm64_jit_pair_remap_codegen_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_pair_remap_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-pair-remap-codegen
        COMMAND uml_arm64_jit_pair_remap_codegen_tests)

    add_executable(uml_arm64_jit_control_remap_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_control_remap_codegen_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(
        uml_arm64_jit_control_remap_codegen_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_control_remap_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-control-remap-codegen
        COMMAND uml_arm64_jit_control_remap_codegen_tests)

    add_executable(uml_arm64_gadget_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_runtime_tests.cpp
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_gadget_runtime_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-runtime
        COMMAND uml_arm64_gadget_runtime_tests)

    add_executable(uml_arm64_gadget_cache_validation_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_cache_validation_tests.cpp
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(
        uml_arm64_gadget_cache_validation_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64
        ${PROJECT_SOURCE_DIR}/darwin/include)
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
    target_include_directories(
        uml_arm64_gadget_inline_fallthrough_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_gadget_inline_fallthrough_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-inline-fallthrough
        COMMAND uml_arm64_gadget_inline_fallthrough_tests)

    add_executable(uml_arm64_gadget_hot_path_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_hot_path_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_gadget_hot_path_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_hot_path_tests PRIVATE
		-Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-hot-path COMMAND uml_arm64_gadget_hot_path_tests)

    add_executable(uml_arm64_gadget_nzcv_residency_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_nzcv_residency_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_gadget_nzcv_residency_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_nzcv_residency_tests PRIVATE
		-Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-nzcv-residency
		COMMAND uml_arm64_gadget_nzcv_residency_tests)

    add_executable(uml_arm64_gadget_fp_environment_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_fp_environment_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_gadget_fp_environment_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_fp_environment_tests PRIVATE
		-Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-fp-environment
		COMMAND uml_arm64_gadget_fp_environment_tests)

    add_executable(uml_arm64_gadget_dependency_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_dependency_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_dependency_reference.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_dependency_memory_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_vector_dependency_tests.cpp
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_link_libraries(uml_arm64_gadget_dependency_tests PRIVATE
        UML::CpuContract uml_arm64_engine_test_support)
    target_include_directories(uml_arm64_gadget_dependency_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_dependency_tests PRIVATE -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-dependencies COMMAND uml_arm64_gadget_dependency_tests)

    # The test's exit hold must be registered before any destructor it
    # outlasts: keep engine state lazy or in libraries linked after it.
    add_executable(uml_arm64_gadget_exit_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_exit_tests.cpp
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_gadget_exit_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_exit_tests PRIVATE -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-exit COMMAND uml_arm64_gadget_exit_tests)

    add_executable(uml_arm64_gadget_link_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_link_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_gadget_link_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_link_tests PRIVATE
		-Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-links COMMAND uml_arm64_gadget_link_tests)

    add_executable(uml_arm64_gadget_indirect_link_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_indirect_link_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_gadget_indirect_link_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_indirect_link_tests PRIVATE
		-Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-indirect-links
		COMMAND uml_arm64_gadget_indirect_link_tests)

    add_executable(uml_gadget_record_table_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/gadget_record_table_tests.cpp)
    target_include_directories(uml_gadget_record_table_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin)
    target_compile_options(uml_gadget_record_table_tests PRIVATE
		-Wall -Wextra -Werror)
    uml_add_unit_test(NAME gadget-record-table
		COMMAND uml_gadget_record_table_tests)
