# SPDX-License-Identifier: GPL-2.0-or-later

# ARM64 white-box tests compose the MMU OBJECT carriers directly. Carry the
# shared-backing archive that MappedBacking references so those raw
# compositions retain the production memory component's link contract.
link_libraries(UML::MemorySharedBacking)
# Tests that enter the runtime without an emulator run route a Context of
# their own.
link_libraries(uml_runtime_test_route)

# The root's warning policy covers C++; these hand-written assembly sources
# opt in to it.
set_source_files_properties(arm64_jit_chain_abi_probe.S arm64-neon-inventory.s
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")

# uml_add_arm64_unit_test(<stem> [NAME <test>] [WITHOUT_HARNESS]
#     [SOURCES <sources>...] [OBJECTS <sources>... | NO_OBJECTS]
#     [LABELS <labels>...])
#
# Builds uml_arm64_<stem>_tests from arm64_<stem>_tests.cpp and any SOURCES,
# composed with the static MMU and gadget engine objects (or OBJECTS instead,
# or none for NO_OBJECTS) and linked to the engine test harness unless
# WITHOUT_HARNESS, then registers it as arm64-<stem with dashes>, or as NAME,
# with any ARGS as its command-line arguments.
function(uml_add_arm64_unit_test STEM)
    cmake_parse_arguments(PARSE_ARGV 1 _arm64_test "WITHOUT_HARNESS;NO_OBJECTS"
        "NAME" "SOURCES;OBJECTS;LABELS;ARGS")
    set(_target uml_arm64_${STEM}_tests)
    if(NOT DEFINED _arm64_test_OBJECTS AND NOT _arm64_test_NO_OBJECTS)
        set(_arm64_test_OBJECTS
            $<TARGET_OBJECTS:uml_static_mmu>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    endif()
    add_executable(${_target}
        ${CMAKE_CURRENT_SOURCE_DIR}/arm64_${STEM}_tests.cpp
        ${_arm64_test_SOURCES}
        ${_arm64_test_OBJECTS})
    target_link_libraries(${_target} PRIVATE UML::BridgeAbi)
    # An image that carries the gadget region lays it out as the products do.
    if("$<TARGET_OBJECTS:uml_arm64_gadgets>" IN_LIST _arm64_test_OBJECTS)
        target_link_libraries(${_target} PRIVATE UML::GadgetRegionLayout)
    endif()
    target_include_directories(${_target} PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    if(NOT _arm64_test_WITHOUT_HARNESS)
        target_link_libraries(${_target} PRIVATE uml_arm64_engine_test_support)
    endif()
    if(NOT DEFINED _arm64_test_NAME)
        string(REPLACE "_" "-" _arm64_test_NAME "arm64-${STEM}")
    endif()
    set(_labels)
    if(DEFINED _arm64_test_LABELS)
        set(_labels LABELS ${_arm64_test_LABELS})
    endif()
    uml_add_unit_test(NAME ${_arm64_test_NAME}
        COMMAND ${_target} ${_arm64_test_ARGS} ${_labels})
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/engine.cmake")

# The guest-SME policy, the host prober, the SME state every SME lane
# shares and the identification registers, through the guest selector's
# entry points.
uml_add_arm64_unit_test(sme_foundation WITHOUT_HARNESS
    OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

# The guest-SVE policy, the non-streaming SVE profile and its predicate
# forms, the state it adds and the identification registers.
uml_add_arm64_unit_test(sve_foundation WITHOUT_HARNESS
    OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

# Instruction-family tests: base, memory, then FP and SIMD.
uml_add_arm64_unit_test(base_arithmetic_allocation)
uml_add_arm64_unit_test(base_bit_allocation)
uml_add_arm64_unit_test(base_multiply_allocation)
uml_add_arm64_unit_test(base_condition_structure)
uml_add_arm64_unit_test(base_min_max_structure)
uml_add_arm64_unit_test(base_control_pc_relative)
uml_add_arm64_unit_test(base_control_direct_branch)
uml_add_arm64_unit_test(base_control_conditional_branch)
uml_add_arm64_unit_test(base_control_compare_test)
uml_add_arm64_unit_test(base_control_register_branch)
uml_add_arm64_unit_test(base_crc_gadget)
uml_add_arm64_unit_test(base_flag_structure)
uml_add_arm64_unit_test(base_barrier_structure)
uml_add_arm64_unit_test(base_hint_structure)
uml_add_arm64_unit_test(base_store_hint_gadget)
uml_add_arm64_unit_test(base_system_register NAME arm64-base-system-registers)
uml_add_arm64_unit_test(base_cache_zero)
uml_add_arm64_unit_test(base_dc_gadget NAME arm64-base-dc_gadget)
uml_add_arm64_unit_test(base_system_operation_gadget
    NAME arm64-base-system_operation_gadget)
uml_add_arm64_unit_test(base_gic_gadget NAME arm64-base-gic_gadget)
uml_add_arm64_unit_test(base_feature_system_gadget
    NAME arm64-base-feature_system_gadget)
uml_add_arm64_unit_test(base_chkfeat_gadget NAME arm64-base-chkfeat_gadget)
uml_add_arm64_unit_test(base_at_gadget NAME arm64-base-at_gadget)
uml_add_arm64_unit_test(base_ic_gadget NAME arm64-base-ic_gadget)
uml_add_arm64_unit_test(base_pstate_gadget)
uml_add_arm64_unit_test(base_range_prefetch)
uml_add_arm64_unit_test(base_mops_gadget)
uml_add_arm64_unit_test(base_immediate_memory_structure)
uml_add_arm64_unit_test(base_load_store_register_structure)
uml_add_arm64_unit_test(base_ordered_behavior)
uml_add_arm64_unit_test(base_ordered_gadget)
uml_add_arm64_unit_test(simd_ordinary_memory_gadget)
uml_add_arm64_unit_test(simd_structure_gadget)
uml_add_arm64_unit_test(simd_baseline_inventory WITHOUT_HARNESS
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/arm64-neon-inventory.s)
uml_add_arm64_unit_test(sme_encoding_table WITHOUT_HARNESS NO_OBJECTS)
uml_add_arm64_unit_test(gadget_simd_destination_residency)
uml_add_arm64_unit_test(simd_widening_gadget)
uml_add_arm64_unit_test(simd_high_narrow_gadget)
uml_add_arm64_unit_test(simd_two_register_gadget)
uml_add_arm64_unit_test(simd_bitwise_gadget)
uml_add_arm64_unit_test(simd_rearrange_gadget_structure
    NAME arm64-simd-rearrange-gadget_structure)
uml_add_arm64_unit_test(basic_floating_allocation)
uml_add_arm64_unit_test(fused_floating_gadget)
uml_add_arm64_unit_test(floating_horizontal_gadget)
uml_add_arm64_unit_test(floating_reciprocal_gadget)
uml_add_arm64_unit_test(floating_complex_gadget)
uml_add_arm64_unit_test(bfloat_conversion_gadget)
uml_add_arm64_unit_test(bfloat_multiply_gadget)
uml_add_arm64_unit_test(bfloat_dot_gadget)
uml_add_arm64_unit_test(bfloat_matrix_gadget)
uml_add_arm64_unit_test(half_fused_long_allocation)
uml_add_arm64_unit_test(simd_aes_gadget)
uml_add_arm64_unit_test(simd_sha3_gadget)
uml_add_arm64_unit_test(simd_pmull_gadget)
uml_add_arm64_unit_test(simd_indexed_gadget)
uml_add_arm64_unit_test(simd_dot_gadget)
uml_add_arm64_unit_test(simd_matrix_gadget)
uml_add_arm64_unit_test(simd_fcvt_precision_gadget)
uml_add_arm64_unit_test(simd_fixed_transform_gadget
    NAME arm64-simd-fixed_transform_gadget)
