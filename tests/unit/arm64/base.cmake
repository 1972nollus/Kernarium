# SPDX-License-Identifier: GPL-2.0-or-later
#
# This family manifest is included by the ARM64 unit-test directory.
    foreach(base_arithmetic_suite IN ITEMS allocation)
        add_executable(
			uml_arm64_base_arithmetic_${base_arithmetic_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_arithmetic_${base_arithmetic_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
        target_include_directories(
			uml_arm64_base_arithmetic_${base_arithmetic_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_arithmetic_${base_arithmetic_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_arithmetic_${base_arithmetic_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        string(REPLACE "_" "-" base_arithmetic_test_name
			"${base_arithmetic_suite}")
        uml_add_unit_test(NAME arm64-base-arithmetic-${base_arithmetic_test_name}
			COMMAND
			uml_arm64_base_arithmetic_${base_arithmetic_suite}_tests)
    endforeach()

    foreach(base_bit_suite IN ITEMS allocation)
        add_executable(uml_arm64_base_bit_${base_bit_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_bit_${base_bit_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
        target_include_directories(
			uml_arm64_base_bit_${base_bit_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_bit_${base_bit_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_bit_${base_bit_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        uml_add_unit_test(NAME arm64-base-bit-${base_bit_suite}
			COMMAND uml_arm64_base_bit_${base_bit_suite}_tests)
    endforeach()

    add_executable(uml_arm64_base_multiply_allocation_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_multiply_allocation_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_multiply_allocation_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_multiply_allocation_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_multiply_allocation_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-multiply-allocation
		COMMAND uml_arm64_base_multiply_allocation_tests)

    foreach(base_condition_suite IN ITEMS structure)
        add_executable(uml_arm64_base_condition_${base_condition_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_condition_${base_condition_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
        target_include_directories(
			uml_arm64_base_condition_${base_condition_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_condition_${base_condition_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_condition_${base_condition_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        string(REPLACE "_" "-" base_condition_test_name
			"${base_condition_suite}")
        uml_add_unit_test(NAME arm64-base-condition-${base_condition_test_name}
			COMMAND
			uml_arm64_base_condition_${base_condition_suite}_tests)
    endforeach()

    foreach(base_min_max_suite IN ITEMS structure)
        add_executable(uml_arm64_base_min_max_${base_min_max_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_min_max_${base_min_max_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
        target_include_directories(
			uml_arm64_base_min_max_${base_min_max_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_min_max_${base_min_max_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_min_max_${base_min_max_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        uml_add_unit_test(NAME arm64-base-min-max-${base_min_max_suite}
			COMMAND uml_arm64_base_min_max_${base_min_max_suite}_tests)
    endforeach()

    add_executable(uml_arm64_base_mte_pointer_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_mte_pointer_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_mte_pointer_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mte_pointer_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_mte_pointer_tests PRIVATE
		uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-mte-pointer
		COMMAND uml_arm64_base_mte_pointer_tests)

    add_executable(uml_arm64_base_mte_tag_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_mte_tag_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_mte_tag_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mte_tag_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_mte_tag_tests PRIVATE
		uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-mte-tag COMMAND uml_arm64_base_mte_tag_tests)

    add_executable(uml_arm64_base_mte_random_tag_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_mte_random_tag_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_mte_random_tag_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mte_random_tag_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_mte_random_tag_tests PRIVATE
		uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-mte-random-tag
		COMMAND uml_arm64_base_mte_random_tag_tests)

    add_executable(uml_arm64_base_cpa_pointer_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_cpa_pointer_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_cpa_pointer_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_cpa_pointer_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_cpa_pointer_tests PRIVATE
		uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-cpa-pointer
		COMMAND uml_arm64_base_cpa_pointer_tests)

    add_executable(uml_arm64_base_cpa_multiply_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_cpa_multiply_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_cpa_multiply_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_cpa_multiply_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_cpa_multiply_tests PRIVATE
		uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-cpa-multiply
		COMMAND uml_arm64_base_cpa_multiply_tests)

    add_executable(uml_arm64_base_cmpbr_immediate_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_cmpbr_immediate_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_cmpbr_immediate_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_cmpbr_immediate_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_cmpbr_immediate_tests PRIVATE
		uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-cmpbr-immediate
		COMMAND uml_arm64_base_cmpbr_immediate_tests)

    add_executable(uml_arm64_base_cmpbr_register_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_cmpbr_register_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_cmpbr_register_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_cmpbr_register_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_cmpbr_register_tests PRIVATE
		uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-cmpbr-register
		COMMAND uml_arm64_base_cmpbr_register_tests)

    foreach(base_control_suite IN ITEMS
		pc_relative
		direct_branch
		conditional_branch
		compare_test
		register_branch)
        add_executable(uml_arm64_base_control_${base_control_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_control_${base_control_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
        target_include_directories(
			uml_arm64_base_control_${base_control_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_control_${base_control_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_control_${base_control_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        string(REPLACE "_" "-" base_control_test_name
			"${base_control_suite}")
        uml_add_unit_test(NAME arm64-base-control-${base_control_test_name}
			COMMAND
			uml_arm64_base_control_${base_control_suite}_tests)
    endforeach()

    foreach(base_crc_suite IN ITEMS gadget)
        add_executable(uml_arm64_base_crc_${base_crc_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_crc_${base_crc_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
        target_include_directories(
			uml_arm64_base_crc_${base_crc_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_crc_${base_crc_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_crc_${base_crc_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        uml_add_unit_test(NAME arm64-base-crc-${base_crc_suite}
			COMMAND uml_arm64_base_crc_${base_crc_suite}_tests)
    endforeach()

    foreach(base_flag_suite IN ITEMS structure)
        add_executable(uml_arm64_base_flag_${base_flag_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_flag_${base_flag_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
        target_include_directories(
			uml_arm64_base_flag_${base_flag_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_flag_${base_flag_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_flag_${base_flag_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        uml_add_unit_test(NAME arm64-base-flag-${base_flag_suite}
			COMMAND uml_arm64_base_flag_${base_flag_suite}_tests)
    endforeach()

    foreach(base_barrier_suite IN ITEMS structure)
        add_executable(uml_arm64_base_barrier_${base_barrier_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_barrier_${base_barrier_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
        target_include_directories(
			uml_arm64_base_barrier_${base_barrier_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_barrier_${base_barrier_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_barrier_${base_barrier_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        uml_add_unit_test(NAME arm64-base-barrier-${base_barrier_suite}
			COMMAND uml_arm64_base_barrier_${base_barrier_suite}_tests)
    endforeach()

    foreach(base_hint_suite IN ITEMS structure)
        add_executable(uml_arm64_base_hint_${base_hint_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_hint_${base_hint_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
        target_include_directories(
			uml_arm64_base_hint_${base_hint_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_hint_${base_hint_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_hint_${base_hint_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        uml_add_unit_test(NAME arm64-base-hint-${base_hint_suite}
			COMMAND uml_arm64_base_hint_${base_hint_suite}_tests)
    endforeach()

    add_executable(uml_arm64_base_hint_stateful_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_hint_stateful_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_hint_stateful_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_hint_stateful_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_hint_stateful_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-hint-stateful
        COMMAND uml_arm64_base_hint_stateful_tests)

    foreach(base_store_hint_suite IN ITEMS gadget)
	add_executable(
	    uml_arm64_base_store_hint_${base_store_hint_suite}_tests
	    ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_store_hint_${base_store_hint_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>
	    $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
	    uml_arm64_base_store_hint_${base_store_hint_suite}_tests PRIVATE
	    ${PROJECT_SOURCE_DIR}/darwin
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_base_store_hint_${base_store_hint_suite}_tests PRIVATE
	    -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_base_store_hint_${base_store_hint_suite}_tests PRIVATE
	    uml_arm64_engine_test_support)
	uml_add_unit_test(NAME arm64-base-store-hint-${base_store_hint_suite}
	    COMMAND
	    uml_arm64_base_store_hint_${base_store_hint_suite}_tests)
    endforeach()

    add_executable(uml_arm64_base_system_register_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_system_register_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_system_register_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_system_register_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_system_register_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-system-registers
		COMMAND uml_arm64_base_system_register_tests)

    add_executable(uml_arm64_base_cache_zero_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_cache_zero_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_cache_zero_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_cache_zero_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_cache_zero_tests PRIVATE
		uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-cache-zero
		COMMAND uml_arm64_base_cache_zero_tests)

	foreach(SYSTEM_OPERATION_TEST IN ITEMS
			dc_gadget
			system_operation_gadget)
		add_executable(uml_arm64_base_${SYSTEM_OPERATION_TEST}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_${SYSTEM_OPERATION_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_base_${SYSTEM_OPERATION_TEST}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_base_${SYSTEM_OPERATION_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_base_${SYSTEM_OPERATION_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-base-${SYSTEM_OPERATION_TEST}
			COMMAND uml_arm64_base_${SYSTEM_OPERATION_TEST}_tests)
	endforeach()

	foreach(GCS_TEST IN ITEMS gadget)
		add_executable(uml_arm64_base_gcs_${GCS_TEST}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_gcs_${GCS_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(uml_arm64_base_gcs_${GCS_TEST}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_base_gcs_${GCS_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(uml_arm64_base_gcs_${GCS_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-base-gcs-${GCS_TEST}
			COMMAND uml_arm64_base_gcs_${GCS_TEST}_tests)
	endforeach()

	foreach(GIC_TEST IN ITEMS gic_gadget)
		add_executable(uml_arm64_base_${GIC_TEST}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_${GIC_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(uml_arm64_base_${GIC_TEST}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_base_${GIC_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(uml_arm64_base_${GIC_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-base-${GIC_TEST}
			COMMAND uml_arm64_base_${GIC_TEST}_tests)
	endforeach()

	foreach(FEATURE_SYSTEM_TEST IN ITEMS feature_system_gadget)
		add_executable(uml_arm64_base_${FEATURE_SYSTEM_TEST}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_${FEATURE_SYSTEM_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_base_${FEATURE_SYSTEM_TEST}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_base_${FEATURE_SYSTEM_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_base_${FEATURE_SYSTEM_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-base-${FEATURE_SYSTEM_TEST}
			COMMAND uml_arm64_base_${FEATURE_SYSTEM_TEST}_tests)
	endforeach()

	foreach(CHKFEAT_TEST IN ITEMS chkfeat_gadget)
		add_executable(uml_arm64_base_${CHKFEAT_TEST}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_${CHKFEAT_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_base_${CHKFEAT_TEST}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_base_${CHKFEAT_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_base_${CHKFEAT_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-base-${CHKFEAT_TEST}
			COMMAND uml_arm64_base_${CHKFEAT_TEST}_tests)
	endforeach()

	foreach(AT_TEST IN ITEMS at_gadget)
		add_executable(uml_arm64_base_${AT_TEST}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_${AT_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(uml_arm64_base_${AT_TEST}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_base_${AT_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(uml_arm64_base_${AT_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-base-${AT_TEST}
			COMMAND uml_arm64_base_${AT_TEST}_tests)
	endforeach()

	foreach(IC_TEST IN ITEMS ic_gadget)
		add_executable(uml_arm64_base_${IC_TEST}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_${IC_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(uml_arm64_base_${IC_TEST}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_base_${IC_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(uml_arm64_base_${IC_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-base-${IC_TEST}
			COMMAND uml_arm64_base_${IC_TEST}_tests)
	endforeach()

	foreach(PSTATE_TEST IN ITEMS gadget)
		add_executable(uml_arm64_base_pstate_${PSTATE_TEST}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_pstate_${PSTATE_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_base_pstate_${PSTATE_TEST}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_base_pstate_${PSTATE_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_base_pstate_${PSTATE_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-base-pstate-${PSTATE_TEST}
			COMMAND uml_arm64_base_pstate_${PSTATE_TEST}_tests)
	endforeach()

    add_executable(uml_arm64_base_pauth_strip_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_pauth_strip_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_pauth_strip_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_strip_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_pauth_strip_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-pauth-strip
		COMMAND uml_arm64_base_pauth_strip_tests)

    add_executable(uml_arm64_base_pauth_code_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_pauth_code_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_pauth_code_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_code_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_pauth_code_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-pauth-code
		COMMAND uml_arm64_base_pauth_code_tests)

    add_executable(uml_arm64_base_pauth_generic_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_pauth_generic_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_pauth_generic_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_generic_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_pauth_generic_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-pauth-generic
		COMMAND uml_arm64_base_pauth_generic_tests)

    add_executable(uml_arm64_base_pauth_branch_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_pauth_branch_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_pauth_branch_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_branch_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_pauth_branch_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-pauth-branch
		COMMAND uml_arm64_base_pauth_branch_tests)

    add_executable(uml_arm64_base_pauth_return_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_pauth_return_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_pauth_return_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_return_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_pauth_return_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-pauth-return
		COMMAND uml_arm64_base_pauth_return_tests)

    add_executable(uml_arm64_base_pauth_load_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_pauth_load_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_pauth_load_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_load_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_pauth_load_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-pauth-load
		COMMAND uml_arm64_base_pauth_load_tests)

    add_executable(uml_arm64_base_pauth_lr_data_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_pauth_lr_data_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_pauth_lr_data_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_lr_data_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_pauth_lr_data_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-pauth-lr-data
		COMMAND uml_arm64_base_pauth_lr_data_tests)

    add_executable(uml_arm64_base_pauth_lr_return_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_pauth_lr_return_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_pauth_lr_return_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_lr_return_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_pauth_lr_return_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-pauth-lr-return
		COMMAND uml_arm64_base_pauth_lr_return_tests)

    add_executable(uml_arm64_base_range_prefetch_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_range_prefetch_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_range_prefetch_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_range_prefetch_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_range_prefetch_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-range-prefetch
		COMMAND uml_arm64_base_range_prefetch_tests)

