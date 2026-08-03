# SPDX-License-Identifier: GPL-2.0-or-later
#
# This family manifest is included by the ARM64 unit-test directory.
    # Compose the test runtime from layer-owned object targets. Only the JIT
    # implementation has a test-compiled variant; all unaffected objects are
    # exactly the production objects.
    set(UML_STATIC_RUNTIME_TEST_OBJECTS
        $<TARGET_OBJECTS:uml_static_runtime>
        $<TARGET_OBJECTS:uml_cpu_engine_factory>
        $<TARGET_OBJECTS:uml_arm64_interpreter_engine>
        $<TARGET_OBJECTS:uml_arm64_gadget_engine>
        $<TARGET_OBJECTS:uml_arm64_jit_engine>
        $<TARGET_OBJECTS:uml_arm64_gadget_executor>
        $<TARGET_OBJECTS:uml_arm64_jit>
        $<TARGET_OBJECTS:uml_arm64_jit_test_support>
        $<TARGET_OBJECTS:uml_static_guest_memory>
        $<TARGET_OBJECTS:uml_runtime_context>)

    add_library(uml_arm64_engine_test_support STATIC
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_engine_test_support.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_instruction_evidence.cpp)
    target_link_libraries(uml_arm64_engine_test_support PRIVATE UML::Emulator)
    target_include_directories(uml_arm64_engine_test_support PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_features(uml_arm64_engine_test_support PUBLIC cxx_std_20)
    target_compile_options(uml_arm64_engine_test_support PRIVATE
        -Wall -Wextra -Werror)

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
        $<TARGET_OBJECTS:uml_static_mmu>)
    target_include_directories(uml_static_mmu_batch_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_features(uml_static_mmu_batch_tests PRIVATE cxx_std_20)
    target_compile_options(uml_static_mmu_batch_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME static-mmu-batch COMMAND uml_static_mmu_batch_tests
        LABELS tsan)

    add_executable(uml_emulator_api_tests ${PROJECT_SOURCE_DIR}/tests/unit/arm64/emulator_api_tests.cpp)
    target_link_libraries(uml_emulator_api_tests PRIVATE UML::Emulator)
    target_include_directories(uml_emulator_api_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_features(uml_emulator_api_tests PRIVATE cxx_std_20)
    target_compile_definitions(uml_emulator_api_tests PRIVATE
        UML_EMULATOR_TEST_HAS_SLIRP=$<BOOL:${UML_BUILD_NETWORK_PROVIDER}>)
    target_compile_options(uml_emulator_api_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME emulator-native-entry-api COMMAND uml_emulator_api_tests)

	add_executable(uml_emulator_options_tests
		${PROJECT_SOURCE_DIR}/tests/unit/emulator_options_tests.cpp)
	target_link_libraries(uml_emulator_options_tests PRIVATE UML::Emulator)
	target_include_directories(uml_emulator_options_tests PRIVATE
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

	foreach(_isolated_engine IN ITEMS interpreter gadget jit factory)
		if(_isolated_engine STREQUAL "interpreter")
			set(_isolated_kind 0)
			set(_isolated_target uml_cpu_interpreter_component)
		elseif(_isolated_engine STREQUAL "gadget")
			set(_isolated_kind 1)
			set(_isolated_target uml_cpu_gadget_component)
		elseif(_isolated_engine STREQUAL "jit")
			set(_isolated_kind 2)
			set(_isolated_target uml_cpu_jit_component)
		else()
			set(_isolated_kind 3)
			set(_isolated_target uml_cpu_engine_component)
		endif()
		add_executable(uml_cpu_${_isolated_engine}_isolated_link_tests
			${PROJECT_SOURCE_DIR}/tests/unit/cpu_adapter_link_tests.cpp)
		target_link_libraries(uml_cpu_${_isolated_engine}_isolated_link_tests
			PRIVATE ${_isolated_target})
		target_include_directories(
			uml_cpu_${_isolated_engine}_isolated_link_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin)
		target_compile_features(uml_cpu_${_isolated_engine}_isolated_link_tests
			PRIVATE cxx_std_20)
		target_compile_definitions(
			uml_cpu_${_isolated_engine}_isolated_link_tests PRIVATE
			UML_ISOLATED_ENGINE_KIND=${_isolated_kind})
		target_compile_options(uml_cpu_${_isolated_engine}_isolated_link_tests
			PRIVATE -Wall -Wextra -Werror)
		uml_add_unit_test(NAME cpu-${_isolated_engine}-isolated-link
			COMMAND uml_cpu_${_isolated_engine}_isolated_link_tests)
	endforeach()

    add_executable(uml_arm64_dispatcher_internal_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_dispatcher_internal_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>)
    target_include_directories(uml_arm64_dispatcher_internal_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_options(uml_arm64_dispatcher_internal_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-dispatcher-internal
        COMMAND uml_arm64_dispatcher_internal_tests)

    add_executable(uml_arm64_floating_exception_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_floating_exception_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>)
    target_include_directories(uml_arm64_floating_exception_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_options(uml_arm64_floating_exception_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-floating-exception
        COMMAND uml_arm64_floating_exception_tests)

    foreach(floating_trap_suite IN ITEMS scalar vector control)
        add_executable(uml_arm64_floating_trap_${floating_trap_suite}_tests
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_floating_trap_${floating_trap_suite}_tests.cpp
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_static_mmu>
            $<TARGET_OBJECTS:uml_arm64_gadgets>)
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
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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

    add_executable(uml_arm64_static_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_static_runtime_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(uml_arm64_static_runtime_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_static_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-static-runtime COMMAND uml_arm64_static_runtime_tests
        LABELS tsan)

    add_executable(uml_arm64_static_boundary_state_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_static_boundary_state_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    target_include_directories(uml_arm64_static_translation_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_static_translation_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-static-translation
        COMMAND uml_arm64_static_translation_tests)

    foreach(JIT_TEST IN ITEMS
            runtime
            capability
            cache_validation
            barrier_codegen
            barrier_runtime
            illegal_runtime
            publication_gate
            reservation_alias)
        add_executable(uml_arm64_jit_${JIT_TEST}_tests
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_${JIT_TEST}_tests.cpp
            $<TARGET_OBJECTS:uml_runtime_guest_selector>
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_static_mmu>
            ${UML_STATIC_RUNTIME_TEST_OBJECTS})
        target_include_directories(uml_arm64_jit_${JIT_TEST}_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(uml_arm64_jit_${JIT_TEST}_tests PRIVATE
            -Wall -Wextra -Werror)
        uml_add_unit_test(NAME arm64-jit-${JIT_TEST}
            COMMAND uml_arm64_jit_${JIT_TEST}_tests)
    endforeach()
    set_property(TEST arm64-jit-publication_gate APPEND PROPERTY LABELS tsan)

    add_executable(uml_arm64_jit_lazy_flags_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_lazy_flags_runtime_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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
        add_executable(
            uml_arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests.cpp
            $<TARGET_OBJECTS:uml_runtime_guest_selector>
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_static_mmu>
            ${UML_STATIC_RUNTIME_TEST_OBJECTS})
        target_include_directories(
            uml_arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests
            PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests
            PRIVATE -Wall -Wextra -Werror)
        uml_add_unit_test(
            NAME arm64-jit-indirect-link-${JIT_INDIRECT_LINK_TEST}
            COMMAND
                uml_arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests)
    endforeach()

    foreach(JIT_CHAIN_ABI_TEST IN ITEMS codegen runtime)
        add_executable(
            uml_arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests.cpp
            $<TARGET_OBJECTS:uml_runtime_guest_selector>
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_static_mmu>
            ${UML_STATIC_RUNTIME_TEST_OBJECTS})
        if(JIT_CHAIN_ABI_TEST STREQUAL "runtime")
            target_sources(
                uml_arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests PRIVATE
                ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_chain_abi_probe.S)
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
    endforeach()

    foreach(JIT_EXACT_RETIREMENT_TEST IN ITEMS codegen runtime)
        add_executable(
            uml_arm64_jit_exact_retirement_${JIT_EXACT_RETIREMENT_TEST}_tests
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_exact_retirement_${JIT_EXACT_RETIREMENT_TEST}_tests.cpp
            $<TARGET_OBJECTS:uml_runtime_guest_selector>
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
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
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
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

    add_executable(uml_arm64_jit_register_remap_behavior_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_register_remap_behavior_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_register_remap_simd_behavior_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_register_remap_behavior_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_register_remap_behavior_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-register-remap-behavior
        COMMAND uml_arm64_jit_register_remap_behavior_tests)

    add_executable(uml_arm64_jit_tlb_miss_preservation_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_tlb_miss_preservation_tests.cpp
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
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
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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

    add_executable(uml_arm64_jit_simd_structure_remap_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_simd_structure_remap_runtime_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_simd_structure_remap_runtime_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_simd_structure_remap_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-simd-structure-remap-runtime
        COMMAND uml_arm64_jit_simd_structure_remap_runtime_tests)

    add_executable(uml_arm64_jit_vector_remap_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_vector_remap_codegen_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_vector_remap_pressure_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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

    add_executable(uml_arm64_jit_table_list_remap_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_table_list_remap_runtime_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_table_list_remap_runtime_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_table_list_remap_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-table-list-remap-runtime
        COMMAND uml_arm64_jit_table_list_remap_runtime_tests)

    add_executable(uml_arm64_jit_gpr_residency_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_gpr_residency_codegen_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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

    add_executable(uml_arm64_jit_gpr_residency_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_gpr_residency_runtime_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_gpr_residency_runtime_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_gpr_residency_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-gpr-residency-runtime
        COMMAND uml_arm64_jit_gpr_residency_runtime_tests)

    add_executable(uml_arm64_jit_pair_remap_codegen_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_pair_remap_codegen_tests.cpp
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
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

    add_executable(uml_arm64_jit_control_remap_behavior_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_control_remap_behavior_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_control_remap_behavior_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_control_remap_behavior_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-jit-control-remap-behavior
        COMMAND uml_arm64_jit_control_remap_behavior_tests)

    if(DARWIN_UML_JIT_GPR_LINK_PROFILE)
        add_executable(uml_arm64_jit_gpr_link_profile_codegen_tests
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_gpr_link_profile_codegen_tests.cpp
            $<TARGET_OBJECTS:uml_runtime_guest_selector>
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_static_mmu>
            ${UML_STATIC_RUNTIME_TEST_OBJECTS})
        target_include_directories(
            uml_arm64_jit_gpr_link_profile_codegen_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_jit_gpr_link_profile_codegen_tests PRIVATE
            -Wall -Wextra -Werror)
        uml_add_unit_test(NAME arm64-jit-gpr-link-profile-codegen
            COMMAND uml_arm64_jit_gpr_link_profile_codegen_tests)

        add_executable(uml_arm64_jit_gpr_link_profile_runtime_tests
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_gpr_link_profile_runtime_tests.cpp
            $<TARGET_OBJECTS:uml_runtime_guest_selector>
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_static_mmu>
            ${UML_STATIC_RUNTIME_TEST_OBJECTS})
        target_include_directories(
            uml_arm64_jit_gpr_link_profile_runtime_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_jit_gpr_link_profile_runtime_tests PRIVATE
            -Wall -Wextra -Werror)
        uml_add_unit_test(NAME arm64-jit-gpr-link-profile-runtime
            COMMAND uml_arm64_jit_gpr_link_profile_runtime_tests)
    endif()

    if(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
        add_executable(uml_arm64_jit_gpr_pressure_profile_tests
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_gpr_pressure_profile_tests.cpp
            $<TARGET_OBJECTS:uml_runtime_guest_selector>
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_static_mmu>
            ${UML_STATIC_RUNTIME_TEST_OBJECTS})
        target_include_directories(
            uml_arm64_jit_gpr_pressure_profile_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_jit_gpr_pressure_profile_tests PRIVATE
            -Wall -Wextra -Werror)
        uml_add_unit_test(NAME arm64-jit-gpr-pressure-profile
            COMMAND uml_arm64_jit_gpr_pressure_profile_tests)
    endif()

    add_executable(uml_arm64_gadget_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_runtime_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_gadget_runtime_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-runtime
        COMMAND uml_arm64_gadget_runtime_tests)

    add_executable(uml_arm64_gadget_hot_path_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_hot_path_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_gadget_hot_path_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_hot_path_tests PRIVATE
		-Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-hot-path COMMAND uml_arm64_gadget_hot_path_tests)

    add_executable(uml_arm64_gadget_nzcv_residency_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_nzcv_residency_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_gadget_nzcv_residency_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_nzcv_residency_tests PRIVATE
		-Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-nzcv-residency
		COMMAND uml_arm64_gadget_nzcv_residency_tests)

    add_executable(uml_arm64_gadget_fp_environment_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_fp_environment_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_gadget_fp_environment_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_fp_environment_tests PRIVATE
		-Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-fp-environment
		COMMAND uml_arm64_gadget_fp_environment_tests)

    add_executable(uml_arm64_gadget_link_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_link_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_gadget_link_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_link_tests PRIVATE
		-Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-links COMMAND uml_arm64_gadget_link_tests)

    add_executable(uml_arm64_gadget_indirect_link_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_indirect_link_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_gadget_indirect_link_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_indirect_link_tests PRIVATE
		-Wall -Wextra -Werror)
    uml_add_unit_test(NAME arm64-gadget-indirect-links
		COMMAND uml_arm64_gadget_indirect_link_tests)
