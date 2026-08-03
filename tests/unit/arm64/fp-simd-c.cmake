# SPDX-License-Identifier: GPL-2.0-or-later
#
# This family manifest is included by the ARM64 unit-test directory.
	foreach(simd_dot_suite IN ITEMS allocation vector indexed portable gadget)
		add_executable(uml_arm64_simd_dot_${simd_dot_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_dot_${simd_dot_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
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

	foreach(simd_fp_indexed_suite IN ITEMS allocation arithmetic extended)
		add_executable(
			uml_arm64_simd_fp_indexed_${simd_fp_indexed_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_fp_indexed_${simd_fp_indexed_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fp_indexed_${simd_fp_indexed_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_fp_indexed_${simd_fp_indexed_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_fp_indexed_${simd_fp_indexed_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-fp-indexed-${simd_fp_indexed_suite}
			COMMAND
			uml_arm64_simd_fp_indexed_${simd_fp_indexed_suite}_tests)
	endforeach()

	foreach(simd_matrix_suite IN ITEMS allocation behavior registers portable
			gadget)
		add_executable(uml_arm64_simd_matrix_${simd_matrix_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_matrix_${simd_matrix_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
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

	foreach(simd_fmulx_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_fmulx_${simd_fmulx_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_fmulx_${simd_fmulx_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fmulx_${simd_fmulx_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_link_libraries(
			uml_arm64_simd_fmulx_${simd_fmulx_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		target_compile_options(
			uml_arm64_simd_fmulx_${simd_fmulx_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		uml_add_unit_test(NAME arm64-simd-fmulx-${simd_fmulx_suite}
			COMMAND uml_arm64_simd_fmulx_${simd_fmulx_suite}_tests)
	endforeach()

	foreach(simd_fabs_fneg_suite IN ITEMS allocation scalar vector)
		add_executable(
			uml_arm64_simd_fabs_fneg_${simd_fabs_fneg_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_fabs_fneg_${simd_fabs_fneg_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fabs_fneg_${simd_fabs_fneg_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_link_libraries(
			uml_arm64_simd_fabs_fneg_${simd_fabs_fneg_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		target_compile_options(
			uml_arm64_simd_fabs_fneg_${simd_fabs_fneg_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		uml_add_unit_test(NAME arm64-simd-fabs-fneg-${simd_fabs_fneg_suite}
			COMMAND
			uml_arm64_simd_fabs_fneg_${simd_fabs_fneg_suite}_tests)
	endforeach()

	foreach(simd_fmov_suite IN ITEMS allocation scalar general)
		add_executable(uml_arm64_simd_fmov_${simd_fmov_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_fmov_${simd_fmov_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fmov_${simd_fmov_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_link_libraries(
			uml_arm64_simd_fmov_${simd_fmov_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		target_compile_options(
			uml_arm64_simd_fmov_${simd_fmov_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		uml_add_unit_test(NAME arm64-simd-fmov-${simd_fmov_suite}
			COMMAND uml_arm64_simd_fmov_${simd_fmov_suite}_tests)
	endforeach()

	foreach(simd_fcsel_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_fcsel_${simd_fcsel_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_fcsel_${simd_fcsel_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fcsel_${simd_fcsel_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_fcsel_${simd_fcsel_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_fcsel_${simd_fcsel_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-fcsel-${simd_fcsel_suite}
			COMMAND uml_arm64_simd_fcsel_${simd_fcsel_suite}_tests)
	endforeach()

	foreach(simd_fcmp_suite IN ITEMS allocation direct conditional fpcr)
		add_executable(uml_arm64_simd_fcmp_${simd_fcmp_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_fcmp_${simd_fcmp_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fcmp_${simd_fcmp_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_fcmp_${simd_fcmp_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_fcmp_${simd_fcmp_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-simd-fcmp-${simd_fcmp_suite}
			COMMAND uml_arm64_simd_fcmp_${simd_fcmp_suite}_tests)
	endforeach()

	foreach(simd_fcvt_precision_suite IN ITEMS allocation exact rounding control
			gadget)
		add_executable(
			uml_arm64_simd_fcvt_precision_${simd_fcvt_precision_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_fcvt_precision_${simd_fcvt_precision_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
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

	foreach(simd_floating_compare_suite IN ITEMS allocation register zero fpcr)
		add_executable(
			uml_arm64_simd_floating_compare_${simd_floating_compare_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_floating_compare_${simd_floating_compare_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_floating_compare_${simd_floating_compare_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_link_libraries(
			uml_arm64_simd_floating_compare_${simd_floating_compare_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		target_compile_options(
			uml_arm64_simd_floating_compare_${simd_floating_compare_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		uml_add_unit_test(NAME arm64-simd-floating-compare-${simd_floating_compare_suite}
			COMMAND
			uml_arm64_simd_floating_compare_${simd_floating_compare_suite}_tests)
	endforeach()

	foreach(simd_fcvt_vector_suite IN ITEMS allocation exact rounding control)
		add_executable(
			uml_arm64_simd_fcvt_vector_${simd_fcvt_vector_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_fcvt_vector_${simd_fcvt_vector_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fcvt_vector_${simd_fcvt_vector_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_link_libraries(
			uml_arm64_simd_fcvt_vector_${simd_fcvt_vector_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		target_compile_options(
			uml_arm64_simd_fcvt_vector_${simd_fcvt_vector_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		uml_add_unit_test(NAME arm64-simd-fcvt-vector-${simd_fcvt_vector_suite}
			COMMAND
			uml_arm64_simd_fcvt_vector_${simd_fcvt_vector_suite}_tests)
	endforeach()

	foreach(simd_fcvtxn_suite IN ITEMS allocation exact rounding control)
		add_executable(uml_arm64_simd_fcvtxn_${simd_fcvtxn_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_fcvtxn_${simd_fcvtxn_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fcvtxn_${simd_fcvtxn_suite}_tests PRIVATE
			${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_link_libraries(
			uml_arm64_simd_fcvtxn_${simd_fcvtxn_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		target_compile_options(uml_arm64_simd_fcvtxn_${simd_fcvtxn_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		uml_add_unit_test(NAME arm64-simd-fcvtxn-${simd_fcvtxn_suite}
			COMMAND uml_arm64_simd_fcvtxn_${simd_fcvtxn_suite}_tests)
	endforeach()

	foreach(floating_to_integer_suite IN ITEMS allocation behavior rounding
			fixed control)
		add_executable(
			uml_arm64_floating_to_integer_${floating_to_integer_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_floating_to_integer_${floating_to_integer_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_floating_to_integer_${floating_to_integer_suite}_tests
			PRIVATE ${PROJECT_SOURCE_DIR}/darwin
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_floating_to_integer_${floating_to_integer_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_floating_to_integer_${floating_to_integer_suite}_tests
			PRIVATE uml_arm64_engine_test_support)
		uml_add_unit_test(NAME arm64-floating-to-integer-${floating_to_integer_suite}
			COMMAND
			uml_arm64_floating_to_integer_${floating_to_integer_suite}_tests)
	endforeach()

	add_executable(uml_arm64_floating_javascript_conversion_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_floating_javascript_conversion_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
		uml_arm64_floating_javascript_conversion_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_floating_javascript_conversion_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_floating_javascript_conversion_tests PRIVATE
		uml_arm64_engine_test_support)
	uml_add_unit_test(NAME arm64-floating-javascript-conversion
		COMMAND uml_arm64_floating_javascript_conversion_tests)

	foreach(simd_sha_suite IN ITEMS sha12_allocation sha1_behavior sha256_behavior
		sha512_allocation sha512_behavior sm_allocation sm3_behavior sm4_behavior
		fixed_transform_portable fixed_transform_gadget)
		add_executable(uml_arm64_simd_${simd_sha_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_${simd_sha_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
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

	foreach(fp8_multiply_matrix_suite IN ITEMS allocation multiply_behavior
		matrix_behavior scale control gadget)
		add_executable(
			uml_arm64_fp8_multiply_matrix_${fp8_multiply_matrix_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_fp8_multiply_matrix_${fp8_multiply_matrix_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
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

	foreach(fp8_dot_suite IN ITEMS allocation behavior scale control gadget)
		add_executable(uml_arm64_fp8_dot_${fp8_dot_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_fp8_dot_${fp8_dot_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
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

	foreach(fp8_convert_up_suite IN ITEMS allocation behavior rounding control
		gadget)
		add_executable(
			uml_arm64_fp8_convert_up_${fp8_convert_up_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_fp8_convert_up_${fp8_convert_up_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
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

	foreach(fp8_convert_down_suite IN ITEMS allocation behavior rounding control)
		add_executable(
			uml_arm64_fp8_convert_down_${fp8_convert_down_suite}_tests
			${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_fp8_convert_down_${fp8_convert_down_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
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
