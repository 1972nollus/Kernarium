# SPDX-License-Identifier: GPL-2.0-or-later
#
# This family manifest is included by the ARM64 unit-test directory.
    foreach(simd_structure_suite IN ITEMS gadget)
		add_executable(
			uml_arm64_simd_structure_${simd_structure_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_structure_${simd_structure_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_simd_structure_${simd_structure_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_structure_${simd_structure_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_structure_${simd_structure_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		string(REPLACE "_" "-" simd_structure_test_name
			"${simd_structure_suite}")
		uml_add_unit_test(NAME arm64-simd-structure-${simd_structure_test_name}
			COMMAND
				uml_arm64_simd_structure_${simd_structure_suite}_tests)
    endforeach()

    foreach(simd_ordered_suite IN ITEMS gadget)
		add_executable(uml_arm64_simd_ordered_${simd_ordered_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_ordered_${simd_ordered_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_simd_ordered_${simd_ordered_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_ordered_${simd_ordered_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_ordered_${simd_ordered_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-ordered-${simd_ordered_suite}
			COMMAND uml_arm64_simd_ordered_${simd_ordered_suite}_tests)
	endforeach()

	add_executable(uml_arm64_simd_baseline_inventory_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_baseline_inventory_tests.cpp
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64-neon-inventory.s
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(uml_arm64_simd_baseline_inventory_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(uml_arm64_simd_baseline_inventory_tests PRIVATE
		-Wall -Wextra -Werror)
	uml_add_unit_test(NAME arm64-simd-baseline-inventory
		COMMAND uml_arm64_simd_baseline_inventory_tests)

	add_executable(uml_arm64_gadget_simd_destination_residency_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_gadget_simd_destination_residency_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
		uml_arm64_gadget_simd_destination_residency_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_definitions(
		uml_arm64_gadget_simd_destination_residency_tests PRIVATE
		UML_GADGET_SIMD_DESTINATION_RESIDENCY=$<BOOL:${UML_GADGET_SIMD_DESTINATION_RESIDENCY}>)
	target_compile_options(
		uml_arm64_gadget_simd_destination_residency_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_gadget_simd_destination_residency_tests PRIVATE
		uml_arm64_engine_test_support)
	uml_add_unit_test(NAME arm64-gadget-simd-destination-residency
		COMMAND uml_arm64_gadget_simd_destination_residency_tests)

	foreach(simd_widening_suite IN ITEMS gadget)
		add_executable(uml_arm64_simd_widening_${simd_widening_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_widening_${simd_widening_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_simd_widening_${simd_widening_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_widening_${simd_widening_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_widening_${simd_widening_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-widening-${simd_widening_suite}
			COMMAND uml_arm64_simd_widening_${simd_widening_suite}_tests)
	endforeach()

	foreach(simd_high_narrow_suite IN ITEMS gadget)
		add_executable(
			uml_arm64_simd_high_narrow_${simd_high_narrow_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_high_narrow_${simd_high_narrow_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_simd_high_narrow_${simd_high_narrow_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_high_narrow_${simd_high_narrow_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_high_narrow_${simd_high_narrow_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-high-narrow-${simd_high_narrow_suite}
			COMMAND
			uml_arm64_simd_high_narrow_${simd_high_narrow_suite}_tests)
	endforeach()

	foreach(simd_two_register_suite IN ITEMS gadget)
		add_executable(uml_arm64_simd_two_register_${simd_two_register_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_two_register_${simd_two_register_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_simd_two_register_${simd_two_register_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_two_register_${simd_two_register_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_two_register_${simd_two_register_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-two-register-${simd_two_register_suite}
			COMMAND uml_arm64_simd_two_register_${simd_two_register_suite}_tests)
	endforeach()

	add_executable(uml_arm64_simd_unallocated_class_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_unallocated_class_tests.cpp
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
		uml_arm64_simd_unallocated_class_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_simd_unallocated_class_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_simd_unallocated_class_tests PRIVATE
		uml_arm64_engine_test_support)
	uml_add_unit_test(NAME arm64-simd-unallocated-class
		COMMAND uml_arm64_simd_unallocated_class_tests)

	foreach(simd_bitwise_suite IN ITEMS gadget)
		add_executable(uml_arm64_simd_bitwise_${simd_bitwise_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_bitwise_${simd_bitwise_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_simd_bitwise_${simd_bitwise_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_bitwise_${simd_bitwise_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_bitwise_${simd_bitwise_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-bitwise-${simd_bitwise_suite}
			COMMAND uml_arm64_simd_bitwise_${simd_bitwise_suite}_tests)
	endforeach()

	foreach(simd_rearrange_suite IN ITEMS gadget_structure)
		add_executable(uml_arm64_simd_rearrange_${simd_rearrange_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_rearrange_${simd_rearrange_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_simd_rearrange_${simd_rearrange_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_rearrange_${simd_rearrange_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_rearrange_${simd_rearrange_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-rearrange-${simd_rearrange_suite}
			COMMAND uml_arm64_simd_rearrange_${simd_rearrange_suite}_tests)
	endforeach()

	foreach(simd_lut_suite IN ITEMS gadget)
		add_executable(uml_arm64_simd_lut_${simd_lut_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_lut_${simd_lut_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_simd_lut_${simd_lut_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_lut_${simd_lut_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_lut_${simd_lut_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-lut-${simd_lut_suite}
			COMMAND uml_arm64_simd_lut_${simd_lut_suite}_tests)
	endforeach()

	foreach(basic_floating_suite IN ITEMS allocation)
		add_executable(uml_arm64_basic_floating_${basic_floating_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_basic_floating_${basic_floating_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_basic_floating_${basic_floating_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_basic_floating_${basic_floating_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_basic_floating_${basic_floating_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-basic-floating-${basic_floating_suite}
			COMMAND uml_arm64_basic_floating_${basic_floating_suite}_tests)
	endforeach()

	foreach(fused_floating_suite IN ITEMS gadget)
		add_executable(uml_arm64_fused_floating_${fused_floating_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_fused_floating_${fused_floating_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_fused_floating_${fused_floating_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_definitions(
			uml_arm64_fused_floating_${fused_floating_suite}_tests PRIVATE
			UML_GADGET_INPLACE_IMMEDIATE=$<BOOL:${UML_GADGET_INPLACE_IMMEDIATE}>)
		target_compile_options(
			uml_arm64_fused_floating_${fused_floating_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_fused_floating_${fused_floating_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-fused-floating-${fused_floating_suite}
			COMMAND uml_arm64_fused_floating_${fused_floating_suite}_tests)
	endforeach()

	foreach(floating_horizontal_suite IN ITEMS gadget)
		add_executable(
			uml_arm64_floating_horizontal_${floating_horizontal_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_floating_horizontal_${floating_horizontal_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_floating_horizontal_${floating_horizontal_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_floating_horizontal_${floating_horizontal_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_floating_horizontal_${floating_horizontal_suite}_tests
			PRIVATE uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-floating-horizontal-${floating_horizontal_suite}
			COMMAND
			uml_arm64_floating_horizontal_${floating_horizontal_suite}_tests)
	endforeach()

	foreach(floating_reciprocal_suite IN ITEMS gadget)
		add_executable(
			uml_arm64_floating_reciprocal_${floating_reciprocal_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_floating_reciprocal_${floating_reciprocal_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_floating_reciprocal_${floating_reciprocal_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_floating_reciprocal_${floating_reciprocal_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_floating_reciprocal_${floating_reciprocal_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-floating-reciprocal-${floating_reciprocal_suite}
			COMMAND
			uml_arm64_floating_reciprocal_${floating_reciprocal_suite}_tests)
	endforeach()

	foreach(floating_complex_suite IN ITEMS gadget)
		add_executable(
			uml_arm64_floating_complex_${floating_complex_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_floating_complex_${floating_complex_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_floating_complex_${floating_complex_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_floating_complex_${floating_complex_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_floating_complex_${floating_complex_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-floating-complex-${floating_complex_suite}
			COMMAND
			uml_arm64_floating_complex_${floating_complex_suite}_tests)
	endforeach()

	foreach(floating_absolute_minmax_suite IN ITEMS gadget)
		add_executable(
			uml_arm64_floating_absolute_minmax_${floating_absolute_minmax_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_floating_absolute_minmax_${floating_absolute_minmax_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_floating_absolute_minmax_${floating_absolute_minmax_suite}_tests
			PRIVATE ${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_floating_absolute_minmax_${floating_absolute_minmax_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_floating_absolute_minmax_${floating_absolute_minmax_suite}_tests
			PRIVATE uml_arm64_engine_test_support)
		uml_add_unit_test(
			NAME arm64-floating-absolute-minmax-${floating_absolute_minmax_suite}
			COMMAND
			uml_arm64_floating_absolute_minmax_${floating_absolute_minmax_suite}_tests)
	endforeach()

