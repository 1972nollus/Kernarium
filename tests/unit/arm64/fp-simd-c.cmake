# SPDX-License-Identifier: GPL-2.0-or-later
#
# This family manifest is included by the ARM64 unit-test directory.
	foreach(simd_dot_suite IN ITEMS gadget)
		add_executable(uml_arm64_simd_dot_${simd_dot_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_dot_${simd_dot_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_simd_dot_${simd_dot_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_dot_${simd_dot_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_dot_${simd_dot_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-dot-${simd_dot_suite}
			COMMAND uml_arm64_simd_dot_${simd_dot_suite}_tests)
	endforeach()

	foreach(simd_matrix_suite IN ITEMS gadget)
		add_executable(uml_arm64_simd_matrix_${simd_matrix_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_matrix_${simd_matrix_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_simd_matrix_${simd_matrix_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_matrix_${simd_matrix_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_matrix_${simd_matrix_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-matrix-${simd_matrix_suite}
			COMMAND uml_arm64_simd_matrix_${simd_matrix_suite}_tests)
	endforeach()

	foreach(simd_fcvt_precision_suite IN ITEMS gadget)
		add_executable(
			uml_arm64_simd_fcvt_precision_${simd_fcvt_precision_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_fcvt_precision_${simd_fcvt_precision_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_simd_fcvt_precision_${simd_fcvt_precision_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_fcvt_precision_${simd_fcvt_precision_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_fcvt_precision_${simd_fcvt_precision_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-fcvt-precision-${simd_fcvt_precision_suite}
			COMMAND
			uml_arm64_simd_fcvt_precision_${simd_fcvt_precision_suite}_tests)
	endforeach()

	foreach(simd_sha_suite IN ITEMS fixed_transform_gadget)
		add_executable(uml_arm64_simd_${simd_sha_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_${simd_sha_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_simd_${simd_sha_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_${simd_sha_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_${simd_sha_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-${simd_sha_suite}
			COMMAND uml_arm64_simd_${simd_sha_suite}_tests)
	endforeach()

	foreach(fp8_multiply_matrix_suite IN ITEMS gadget)
		add_executable(
			uml_arm64_fp8_multiply_matrix_${fp8_multiply_matrix_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_fp8_multiply_matrix_${fp8_multiply_matrix_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_fp8_multiply_matrix_${fp8_multiply_matrix_suite}_tests
			PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_fp8_multiply_matrix_${fp8_multiply_matrix_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_fp8_multiply_matrix_${fp8_multiply_matrix_suite}_tests
			PRIVATE uml_arm64_engine_test_support)
		string(REPLACE "_" "-"
			fp8_multiply_matrix_test_name
			"${fp8_multiply_matrix_suite}")
		uml_add_unit_test(
			NAME arm64-fp8-multiply-matrix-${fp8_multiply_matrix_test_name}
			COMMAND
			uml_arm64_fp8_multiply_matrix_${fp8_multiply_matrix_suite}_tests)
	endforeach()

	foreach(fp8_dot_suite IN ITEMS gadget)
		add_executable(uml_arm64_fp8_dot_${fp8_dot_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_fp8_dot_${fp8_dot_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_fp8_dot_${fp8_dot_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_fp8_dot_${fp8_dot_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_fp8_dot_${fp8_dot_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-fp8-dot-${fp8_dot_suite}
			COMMAND uml_arm64_fp8_dot_${fp8_dot_suite}_tests)
	endforeach()

	foreach(fp8_convert_up_suite IN ITEMS gadget)
		add_executable(
			uml_arm64_fp8_convert_up_${fp8_convert_up_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_fp8_convert_up_${fp8_convert_up_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_fp8_convert_up_${fp8_convert_up_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_fp8_convert_up_${fp8_convert_up_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_fp8_convert_up_${fp8_convert_up_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-fp8-convert-up-${fp8_convert_up_suite}
			COMMAND
			uml_arm64_fp8_convert_up_${fp8_convert_up_suite}_tests)
	endforeach()

	foreach(fp8_convert_down_suite IN ITEMS allocation)
		add_executable(
			uml_arm64_fp8_convert_down_${fp8_convert_down_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_fp8_convert_down_${fp8_convert_down_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>
			$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
		target_include_directories(
			uml_arm64_fp8_convert_down_${fp8_convert_down_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_fp8_convert_down_${fp8_convert_down_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_fp8_convert_down_${fp8_convert_down_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-fp8-convert-down-${fp8_convert_down_suite}
			COMMAND
			uml_arm64_fp8_convert_down_${fp8_convert_down_suite}_tests)
	endforeach()
