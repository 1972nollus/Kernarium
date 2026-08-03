# SPDX-License-Identifier: GPL-2.0-or-later
#
# This family manifest is included by the ARM64 unit-test directory.
	foreach(bfloat_conversion_suite IN ITEMS allocation behavior control gadget)
		add_executable(
			uml_arm64_bfloat_conversion_${bfloat_conversion_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_bfloat_conversion_${bfloat_conversion_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_bfloat_conversion_${bfloat_conversion_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_bfloat_conversion_${bfloat_conversion_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_bfloat_conversion_${bfloat_conversion_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-bfloat-conversion-${bfloat_conversion_suite}
			COMMAND
			uml_arm64_bfloat_conversion_${bfloat_conversion_suite}_tests)
	endforeach()

	foreach(bfloat_multiply_suite IN ITEMS allocation vector indexed control
			gadget)
		add_executable(
			uml_arm64_bfloat_multiply_${bfloat_multiply_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_bfloat_multiply_${bfloat_multiply_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_bfloat_multiply_${bfloat_multiply_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_bfloat_multiply_${bfloat_multiply_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_bfloat_multiply_${bfloat_multiply_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-bfloat-multiply-${bfloat_multiply_suite}
			COMMAND
			uml_arm64_bfloat_multiply_${bfloat_multiply_suite}_tests)
	endforeach()

	foreach(bfloat_dot_suite IN ITEMS allocation vector indexed control gadget)
		add_executable(uml_arm64_bfloat_dot_${bfloat_dot_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_bfloat_dot_${bfloat_dot_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(uml_arm64_bfloat_dot_${bfloat_dot_suite}_tests
			PRIVATE ${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_bfloat_dot_${bfloat_dot_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_bfloat_dot_${bfloat_dot_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-bfloat-dot-${bfloat_dot_suite}
			COMMAND uml_arm64_bfloat_dot_${bfloat_dot_suite}_tests)
	endforeach()

	foreach(bfloat_matrix_suite IN ITEMS allocation behavior control gadget)
		add_executable(uml_arm64_bfloat_matrix_${bfloat_matrix_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_bfloat_matrix_${bfloat_matrix_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_bfloat_matrix_${bfloat_matrix_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_bfloat_matrix_${bfloat_matrix_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_bfloat_matrix_${bfloat_matrix_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-bfloat-matrix-${bfloat_matrix_suite}
			COMMAND uml_arm64_bfloat_matrix_${bfloat_matrix_suite}_tests)
	endforeach()

	foreach(half_fused_long_suite IN ITEMS allocation vector indexed control
			reserved gadget)
		add_executable(uml_arm64_half_fused_long_${half_fused_long_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_half_fused_long_${half_fused_long_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_half_fused_long_${half_fused_long_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_half_fused_long_${half_fused_long_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_half_fused_long_${half_fused_long_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-half-fused-long-${half_fused_long_suite}
			COMMAND uml_arm64_half_fused_long_${half_fused_long_suite}_tests)
	endforeach()

	foreach(half_matrix_suite IN ITEMS allocation non_widening widening control
			reserved gadget)
		add_executable(uml_arm64_half_matrix_${half_matrix_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_half_matrix_${half_matrix_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_half_matrix_${half_matrix_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_half_matrix_${half_matrix_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_half_matrix_${half_matrix_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-half-matrix-${half_matrix_suite}
			COMMAND uml_arm64_half_matrix_${half_matrix_suite}_tests)
	endforeach()

	foreach(half_dot_suite IN ITEMS allocation vector indexed control reserved
			gadget)
		add_executable(uml_arm64_half_dot_${half_dot_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_half_dot_${half_dot_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(uml_arm64_half_dot_${half_dot_suite}_tests
			PRIVATE ${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_half_dot_${half_dot_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		target_link_libraries(uml_arm64_half_dot_${half_dot_suite}_tests
			PRIVATE uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-half-dot-${half_dot_suite}
			COMMAND uml_arm64_half_dot_${half_dot_suite}_tests)
	endforeach()

	foreach(simd_extract_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_extract_${simd_extract_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_extract_${simd_extract_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_extract_${simd_extract_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_extract_${simd_extract_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_extract_${simd_extract_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-extract-${simd_extract_suite}
			COMMAND uml_arm64_simd_extract_${simd_extract_suite}_tests)
	endforeach()

	foreach(simd_aes_suite IN ITEMS
			allocation behavior control reserved gadget)
		add_executable(uml_arm64_simd_aes_${simd_aes_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_aes_${simd_aes_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_aes_${simd_aes_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_aes_${simd_aes_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_aes_${simd_aes_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-aes-${simd_aes_suite}
			COMMAND uml_arm64_simd_aes_${simd_aes_suite}_tests)
	endforeach()

	foreach(simd_sha3_suite IN ITEMS allocation rotation_allocation behavior
			reserved gadget)
		add_executable(uml_arm64_simd_sha3_${simd_sha3_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_sha3_${simd_sha3_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_sha3_${simd_sha3_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_sha3_${simd_sha3_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_sha3_${simd_sha3_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-sha3-${simd_sha3_suite}
			COMMAND uml_arm64_simd_sha3_${simd_sha3_suite}_tests)
	endforeach()

	foreach(simd_pmull_suite IN ITEMS allocation matrix edge portable gadget)
		add_executable(uml_arm64_simd_pmull_${simd_pmull_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_pmull_${simd_pmull_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_pmull_${simd_pmull_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_pmull_${simd_pmull_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_pmull_${simd_pmull_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-pmull-${simd_pmull_suite}
			COMMAND uml_arm64_simd_pmull_${simd_pmull_suite}_tests)
	endforeach()

	foreach(simd_addp_scalar_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_addp_scalar_${simd_addp_scalar_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_addp_scalar_${simd_addp_scalar_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_addp_scalar_${simd_addp_scalar_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_addp_scalar_${simd_addp_scalar_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_addp_scalar_${simd_addp_scalar_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-addp-scalar-${simd_addp_scalar_suite}
			COMMAND uml_arm64_simd_addp_scalar_${simd_addp_scalar_suite}_tests)
	endforeach()

	foreach(simd_lane_move_suite IN ITEMS allocation duplicate insert gpr)
		add_executable(uml_arm64_simd_lane_move_${simd_lane_move_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_lane_move_${simd_lane_move_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_lane_move_${simd_lane_move_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_lane_move_${simd_lane_move_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_lane_move_${simd_lane_move_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-lane-move-${simd_lane_move_suite}
			COMMAND uml_arm64_simd_lane_move_${simd_lane_move_suite}_tests)
	endforeach()

	foreach(simd_shift_immediate_suite IN ITEMS allocation right left narrow)
		add_executable(
			uml_arm64_simd_shift_immediate_${simd_shift_immediate_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_shift_immediate_${simd_shift_immediate_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_shift_immediate_${simd_shift_immediate_suite}_tests
			PRIVATE ${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_shift_immediate_${simd_shift_immediate_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_shift_immediate_${simd_shift_immediate_suite}_tests
			PRIVATE uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-shift-immediate-${simd_shift_immediate_suite}
			COMMAND
			uml_arm64_simd_shift_immediate_${simd_shift_immediate_suite}_tests)
	endforeach()

	foreach(simd_shift_left_long_suite IN ITEMS allocation behavior)
		add_executable(
			uml_arm64_simd_shift_left_long_${simd_shift_left_long_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_shift_left_long_${simd_shift_left_long_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_shift_left_long_${simd_shift_left_long_suite}_tests
			PRIVATE ${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_shift_left_long_${simd_shift_left_long_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_shift_left_long_${simd_shift_left_long_suite}_tests
			PRIVATE uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-shift-left-long-${simd_shift_left_long_suite}
			COMMAND
			uml_arm64_simd_shift_left_long_${simd_shift_left_long_suite}_tests)
	endforeach()

	foreach(simd_indexed_suite IN ITEMS
			allocation
			reserved
			same
			long
			vector_long
			saturating_long
			vector_saturating_long
			saturating_high
			rdm
			vector_rdm
			gadget)
		add_executable(uml_arm64_simd_indexed_${simd_indexed_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_indexed_${simd_indexed_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_indexed_${simd_indexed_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_indexed_${simd_indexed_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_indexed_${simd_indexed_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-indexed-${simd_indexed_suite}
			COMMAND uml_arm64_simd_indexed_${simd_indexed_suite}_tests)
	endforeach()

