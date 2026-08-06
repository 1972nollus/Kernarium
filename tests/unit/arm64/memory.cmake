# SPDX-License-Identifier: GPL-2.0-or-later
#
# This family manifest is included by the ARM64 unit-test directory.
    foreach(base_ls64_suite IN ITEMS allocation behavior fault gadget)
	add_executable(uml_arm64_base_ls64_${base_ls64_suite}_tests
	    ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_ls64_${base_ls64_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_arm64_dispatch>
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>
	    $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
	    uml_arm64_base_ls64_${base_ls64_suite}_tests PRIVATE
	    ${PROJECT_SOURCE_DIR}/darwin
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_base_ls64_${base_ls64_suite}_tests PRIVATE
	    -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_base_ls64_${base_ls64_suite}_tests PRIVATE
	    uml_arm64_engine_test_support)
	string(REPLACE "_" "-" base_ls64_test_name "${base_ls64_suite}")
	uml_add_unit_test(NAME arm64-base-ls64-${base_ls64_test_name}
	    COMMAND uml_arm64_base_ls64_${base_ls64_suite}_tests)
    endforeach()

    foreach(base_mops_suite IN ITEMS
	    allocation copy_forward copy set set_tag set_tag_fault gadget)
	add_executable(uml_arm64_base_mops_${base_mops_suite}_tests
	    ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_mops_${base_mops_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_arm64_dispatch>
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>
	    $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
	    uml_arm64_base_mops_${base_mops_suite}_tests PRIVATE
	    ${PROJECT_SOURCE_DIR}/darwin
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_base_mops_${base_mops_suite}_tests PRIVATE
	    -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_base_mops_${base_mops_suite}_tests PRIVATE
	    uml_arm64_engine_test_support)
	string(REPLACE "_" "-" base_mops_test_name "${base_mops_suite}")
	uml_add_unit_test(NAME arm64-base-mops-${base_mops_test_name}
	    COMMAND uml_arm64_base_mops_${base_mops_suite}_tests)
    endforeach()

    add_executable(uml_arm64_base_mte_tag_load_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_mte_tag_load_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_mte_tag_load_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mte_tag_load_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_mte_tag_load_tests PRIVATE
		uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-mte-tag-load
		COMMAND uml_arm64_base_mte_tag_load_tests)

    add_executable(uml_arm64_base_mte_tag_store_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_mte_tag_store_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_mte_tag_store_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mte_tag_store_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_mte_tag_store_tests PRIVATE
		uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-mte-tag-store
		COMMAND uml_arm64_base_mte_tag_store_tests)

    foreach(base_mte_data_suite IN ITEMS zero_store stgp)
	add_executable(uml_arm64_base_mte_${base_mte_data_suite}_tests
	    ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_mte_${base_mte_data_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_arm64_dispatch>
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>
	    $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
	    uml_arm64_base_mte_${base_mte_data_suite}_tests PRIVATE
	    ${PROJECT_SOURCE_DIR}/darwin
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_base_mte_${base_mte_data_suite}_tests PRIVATE
	    -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_base_mte_${base_mte_data_suite}_tests PRIVATE
	    uml_arm64_engine_test_support)
	uml_add_unit_test(NAME arm64-base-mte-${base_mte_data_suite}
	    COMMAND uml_arm64_base_mte_${base_mte_data_suite}_tests)
    endforeach()

    foreach(base_mte_multiple_suite IN ITEMS load store gadget)
	add_executable(
	    uml_arm64_base_mte_multiple_${base_mte_multiple_suite}_tests
	    ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_mte_multiple_${base_mte_multiple_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_arm64_dispatch>
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>
	    $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
	    uml_arm64_base_mte_multiple_${base_mte_multiple_suite}_tests PRIVATE
	    ${PROJECT_SOURCE_DIR}/darwin
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_base_mte_multiple_${base_mte_multiple_suite}_tests PRIVATE
	    -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_base_mte_multiple_${base_mte_multiple_suite}_tests PRIVATE
	    uml_arm64_engine_test_support)
	uml_add_unit_test(NAME arm64-base-mte-multiple-${base_mte_multiple_suite}
	    COMMAND
	    uml_arm64_base_mte_multiple_${base_mte_multiple_suite}_tests)
    endforeach()

    foreach(literal_load_suite allocation behavior fault)
	add_executable(uml_arm64_literal_load_${literal_load_suite}_tests
	    ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_literal_load_${literal_load_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_arm64_dispatch>
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>
	    $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
	    uml_arm64_literal_load_${literal_load_suite}_tests PRIVATE
	    ${PROJECT_SOURCE_DIR}/darwin
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_literal_load_${literal_load_suite}_tests PRIVATE
	    -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_literal_load_${literal_load_suite}_tests PRIVATE
	    uml_arm64_engine_test_support)
	uml_add_unit_test(NAME arm64-literal-load-${literal_load_suite}
	    COMMAND uml_arm64_literal_load_${literal_load_suite}_tests)
    endforeach()

    foreach(immediate_memory_suite allocation offset behavior fault structure)
	add_executable(uml_arm64_base_immediate_memory_${immediate_memory_suite}_tests
	    ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_immediate_memory_${immediate_memory_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_arm64_dispatch>
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>
	    $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
	    uml_arm64_base_immediate_memory_${immediate_memory_suite}_tests PRIVATE
	    ${PROJECT_SOURCE_DIR}/darwin
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_base_immediate_memory_${immediate_memory_suite}_tests PRIVATE
	    -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_base_immediate_memory_${immediate_memory_suite}_tests PRIVATE
	    uml_arm64_engine_test_support)
	uml_add_unit_test(NAME arm64-base-immediate-memory-${immediate_memory_suite}
	    COMMAND uml_arm64_base_immediate_memory_${immediate_memory_suite}_tests)
    endforeach()

    add_executable(uml_arm64_base_load_store_register_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_load_store_register_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_load_store_register_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_load_store_register_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_load_store_register_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-load-store-register
		COMMAND uml_arm64_base_load_store_register_tests)

    add_executable(uml_arm64_base_load_store_register_allocation_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_load_store_register_allocation_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(
        uml_arm64_base_load_store_register_allocation_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_base_load_store_register_allocation_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(
        uml_arm64_base_load_store_register_allocation_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-load-store-register-allocation
		COMMAND uml_arm64_base_load_store_register_allocation_tests)

    foreach(load_store_register_suite fault prefetch structure)
	add_executable(
	    uml_arm64_base_load_store_register_${load_store_register_suite}_tests
	    ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_load_store_register_${load_store_register_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_arm64_dispatch>
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>
	    $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
	    uml_arm64_base_load_store_register_${load_store_register_suite}_tests
	    PRIVATE
	    ${PROJECT_SOURCE_DIR}/darwin
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_base_load_store_register_${load_store_register_suite}_tests
	    PRIVATE -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_base_load_store_register_${load_store_register_suite}_tests
	    PRIVATE uml_arm64_engine_test_support)
	uml_add_unit_test(NAME arm64-base-load-store-register-${load_store_register_suite}
	    COMMAND
	    uml_arm64_base_load_store_register_${load_store_register_suite}_tests)
    endforeach()

    add_executable(uml_arm64_base_load_store_pair_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_load_store_pair_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_load_store_pair_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_load_store_pair_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_load_store_pair_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-load-store-pair
		COMMAND uml_arm64_base_load_store_pair_tests)

    foreach(lsui_pair_suite IN ITEMS allocation behavior fault gadget)
	add_executable(uml_arm64_base_lsui_pair_${lsui_pair_suite}_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_lsui_pair_${lsui_pair_suite}_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
		uml_arm64_base_lsui_pair_${lsui_pair_suite}_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_base_lsui_pair_${lsui_pair_suite}_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_base_lsui_pair_${lsui_pair_suite}_tests PRIVATE
		uml_arm64_engine_test_support)
	uml_add_unit_test(NAME arm64-base-lsui-pair-${lsui_pair_suite}
		COMMAND uml_arm64_base_lsui_pair_${lsui_pair_suite}_tests)
    endforeach()

    foreach(ordered_suite IN ITEMS allocation behavior fault gadget)
	add_executable(uml_arm64_base_ordered_${ordered_suite}_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_ordered_${ordered_suite}_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
		uml_arm64_base_ordered_${ordered_suite}_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_base_ordered_${ordered_suite}_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_base_ordered_${ordered_suite}_tests PRIVATE
		uml_arm64_engine_test_support)
	uml_add_unit_test(NAME arm64-base-ordered-${ordered_suite}
		COMMAND uml_arm64_base_ordered_${ordered_suite}_tests)
    endforeach()

    add_executable(uml_arm64_base_exclusive_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_exclusive_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_exclusive_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_exclusive_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_exclusive_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-exclusive COMMAND uml_arm64_base_exclusive_tests)

    add_executable(uml_arm64_base_exclusive_pair_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_exclusive_pair_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_exclusive_pair_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_exclusive_pair_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_exclusive_pair_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-exclusive-pair
		COMMAND uml_arm64_base_exclusive_pair_tests)

    add_executable(uml_arm64_base_compare_swap_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_compare_swap_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_compare_swap_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_compare_swap_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_compare_swap_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-compare-swap
		COMMAND uml_arm64_base_compare_swap_tests)

    add_executable(uml_arm64_base_compare_swap_pair_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_compare_swap_pair_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_compare_swap_pair_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_compare_swap_pair_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_compare_swap_pair_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-compare-swap-pair
		COMMAND uml_arm64_base_compare_swap_pair_tests)

    add_executable(uml_arm64_base_atomic_bit_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_atomic_bit_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_atomic_bit_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_atomic_bit_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_atomic_bit_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-atomic-bit
		COMMAND uml_arm64_base_atomic_bit_tests)

    add_executable(uml_arm64_base_atomic_minmax_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_atomic_minmax_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_atomic_minmax_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_atomic_minmax_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_atomic_minmax_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-atomic-minmax
		COMMAND uml_arm64_base_atomic_minmax_tests)

    add_executable(uml_arm64_base_atomic_swap_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_atomic_swap_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_atomic_swap_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_atomic_swap_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_atomic_swap_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-atomic-swap
		COMMAND uml_arm64_base_atomic_swap_tests)

    foreach(lse128_pair_suite IN ITEMS allocation behavior fault gadget)
	add_executable(uml_arm64_base_lse128_pair_${lse128_pair_suite}_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_lse128_pair_${lse128_pair_suite}_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
		uml_arm64_base_lse128_pair_${lse128_pair_suite}_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_base_lse128_pair_${lse128_pair_suite}_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_base_lse128_pair_${lse128_pair_suite}_tests PRIVATE
		uml_arm64_engine_test_support)
	uml_add_unit_test(NAME arm64-base-lse128-pair-${lse128_pair_suite}
		COMMAND uml_arm64_base_lse128_pair_${lse128_pair_suite}_tests)
    endforeach()

    foreach(atomic_floating_suite IN ITEMS allocation bfloat precision
			ordering_alias fault gadget)
	add_executable(uml_arm64_atomic_floating_${atomic_floating_suite}_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_atomic_floating_${atomic_floating_suite}_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
		uml_arm64_atomic_floating_${atomic_floating_suite}_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_atomic_floating_${atomic_floating_suite}_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_atomic_floating_${atomic_floating_suite}_tests PRIVATE
		uml_arm64_engine_test_support)
	uml_add_unit_test(NAME arm64-atomic-floating-${atomic_floating_suite}
		COMMAND uml_arm64_atomic_floating_${atomic_floating_suite}_tests)
    endforeach()

    foreach(rcw_suite scalar pair allocation fault gadget)
	add_executable(uml_arm64_base_rcw_${rcw_suite}_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_rcw_${rcw_suite}_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
		uml_arm64_base_rcw_${rcw_suite}_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_base_rcw_${rcw_suite}_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_base_rcw_${rcw_suite}_tests PRIVATE
		uml_arm64_engine_test_support)
	uml_add_unit_test(NAME arm64-base-rcw-${rcw_suite}
		COMMAND uml_arm64_base_rcw_${rcw_suite}_tests)
    endforeach()

    add_executable(uml_arm64_base_clear_exclusive_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_clear_exclusive_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_base_clear_exclusive_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_clear_exclusive_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_clear_exclusive_tests PRIVATE
		uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-base-clear-exclusive
		COMMAND uml_arm64_base_clear_exclusive_tests)

    foreach(ordered_pair_suite IN ITEMS allocation behavior fault gadget)
	add_executable(uml_arm64_base_ordered_pair_${ordered_pair_suite}_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_base_ordered_pair_${ordered_pair_suite}_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
		uml_arm64_base_ordered_pair_${ordered_pair_suite}_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_base_ordered_pair_${ordered_pair_suite}_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_base_ordered_pair_${ordered_pair_suite}_tests PRIVATE
		uml_arm64_engine_test_support)
	uml_add_unit_test(NAME arm64-base-ordered-pair-${ordered_pair_suite}
		COMMAND uml_arm64_base_ordered_pair_${ordered_pair_suite}_tests)
    endforeach()

    foreach(simd_ordinary_memory_suite IN ITEMS
		allocation immediate register pair fault tag gadget)
	add_executable(
		uml_arm64_simd_ordinary_memory_${simd_ordinary_memory_suite}_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_ordinary_memory_${simd_ordinary_memory_suite}_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
	target_include_directories(
		uml_arm64_simd_ordinary_memory_${simd_ordinary_memory_suite}_tests
		PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_simd_ordinary_memory_${simd_ordinary_memory_suite}_tests
		PRIVATE -Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_simd_ordinary_memory_${simd_ordinary_memory_suite}_tests
		PRIVATE uml_arm64_engine_test_support)
	string(REPLACE "_" "-" simd_ordinary_memory_test_name
		"${simd_ordinary_memory_suite}")
	uml_add_unit_test(
		NAME arm64-simd-ordinary-memory-${simd_ordinary_memory_test_name}
		COMMAND
			uml_arm64_simd_ordinary_memory_${simd_ordinary_memory_suite}_tests)
    endforeach()

    add_executable(uml_arm64_simd_load_store_pair_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_simd_load_store_pair_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>
		$<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_include_directories(uml_arm64_simd_load_store_pair_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_simd_load_store_pair_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_simd_load_store_pair_tests PRIVATE
        uml_arm64_engine_test_support)
    uml_add_unit_test(NAME arm64-simd-load-store-pair
		COMMAND uml_arm64_simd_load_store_pair_tests)

