# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_arm64_dispatch OBJECT
    cpu/arm64/dispatcher.cpp
    cpu/arm64/instructions/control_flow.cpp
    cpu/arm64/instructions/crc32.cpp
    cpu/arm64/instructions/floating_point.cpp
    cpu/arm64/instructions/integer.cpp
    cpu/arm64/instructions/integer/arithmetic.cpp
    cpu/arm64/instructions/integer/cpa.cpp
    cpu/arm64/instructions/integer/mte.cpp
    cpu/arm64/instructions/load_store.cpp
    cpu/arm64/instructions/simd.cpp
    cpu/arm64/instructions/simd/complete.cpp
    cpu/arm64/instructions/simd/conversion.cpp
    cpu/arm64/instructions/simd/crypto.cpp
    cpu/arm64/instructions/simd/floating_point.cpp
    cpu/arm64/instructions/simd/fp8.cpp
    cpu/arm64/instructions/simd/narrowing.cpp
    cpu/arm64/instructions/simd/rearrange.cpp
    cpu/arm64/instructions/simd/shift.cpp)

target_include_directories(uml_arm64_dispatch PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_arm64_dispatch PUBLIC cxx_std_20)
target_compile_options(uml_arm64_dispatch PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_arm64_dispatch PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_arm64_gadgets OBJECT
    cpu/arm64/gadgets/program.cpp
    cpu/arm64/gadgets/tlb.cpp
    cpu/arm64/gadgets/engine.S)
if(UML_ENABLE_GADGET_INTERPRETER_RUNPATH)
    target_sources(uml_arm64_gadgets PRIVATE
        cpu/arm64/gadgets/decoded.cpp)
endif()
target_include_directories(uml_arm64_gadgets PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_arm64_gadgets PUBLIC cxx_std_20)
target_compile_options(uml_arm64_gadgets PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>)
target_compile_definitions(uml_arm64_gadgets PRIVATE
    UML_ENABLE_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
set_target_properties(uml_arm64_gadgets PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_static_mmu OBJECT
    ${PROJECT_SOURCE_DIR}/darwin/runtime/mmu/address_space.cpp)
target_include_directories(uml_static_mmu PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include)
target_compile_features(uml_static_mmu PUBLIC cxx_std_20)
target_compile_options(uml_static_mmu PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_static_mmu PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_static_runtime OBJECT
    runtime/static_guest.cpp
    runtime/static/gadget_executor.cpp
    runtime/static/guest_memory.cpp)

target_include_directories(uml_static_runtime PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_static_runtime PUBLIC cxx_std_20)
target_compile_options(uml_static_runtime PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_static_runtime PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_native_mmu OBJECT
    ${PROJECT_SOURCE_DIR}/darwin/runtime/mmu/native_address_space.cpp)
target_include_directories(uml_native_mmu PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/runtime/mmu)
target_compile_features(uml_native_mmu PUBLIC cxx_std_20)
target_compile_options(uml_native_mmu PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_native_mmu PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_backend STATIC
    src/backend.cpp
    runtime/guest_selector.cpp
    ${PROJECT_SOURCE_DIR}/darwin/network/device.cpp
    ${PROJECT_SOURCE_DIR}/darwin/runtime/bridge.cpp
    ${PROJECT_SOURCE_DIR}/darwin/runtime/mmu/static_api.cpp
    $<TARGET_OBJECTS:uml_arm64_dispatch>
    $<TARGET_OBJECTS:uml_arm64_gadgets>
    $<TARGET_OBJECTS:uml_static_mmu>
    $<TARGET_OBJECTS:uml_static_runtime>
    $<TARGET_OBJECTS:uml_native_mmu>)

add_library(UML::Backend ALIAS uml_backend)

target_include_directories(uml_backend
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${PROJECT_SOURCE_DIR}/darwin/network
        ${PROJECT_SOURCE_DIR}/darwin/runtime/mmu
        ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(uml_backend PUBLIC cxx_std_20)
target_compile_options(uml_backend PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(uml_backend PRIVATE
    UML_APPLE_PLATFORM_NAME="${UML_APPLE_PLATFORM}"
    UML_BACKEND_HAS_KERNEL_RUNNER=$<BOOL:${UML_BUILD_KERNEL_RUNNER}>
    UML_BACKEND_HAS_SLIRP=$<BOOL:${UML_BUILD_KERNEL_RUNNER}>)

set_target_properties(uml_backend PROPERTIES
    OUTPUT_NAME uml_backend
    POSITION_INDEPENDENT_CODE ON
    PUBLIC_HEADER include/uml/backend.h)

if(UML_BUILD_KERNEL_RUNNER)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SLIRP REQUIRED slirp)
    pkg_check_modules(GLIB REQUIRED glib-2.0)
    pkg_check_modules(PCRE2 REQUIRED libpcre2-8)

    pkg_get_variable(SLIRP_LIBDIR slirp libdir)
    pkg_get_variable(GLIB_LIBDIR glib-2.0 libdir)
    pkg_get_variable(PCRE2_LIBDIR libpcre2-8 libdir)

    target_sources(uml_backend PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/loader/elf_loader.cpp)

    add_library(uml_slirp_network OBJECT
        ${PROJECT_SOURCE_DIR}/darwin/network/slirp/backend.cpp
        ${PROJECT_SOURCE_DIR}/darwin/network/slirp/event_loop.cpp
        ${PROJECT_SOURCE_DIR}/darwin/network/slirp/frame_queue.cpp)
    target_include_directories(uml_slirp_network PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${PROJECT_SOURCE_DIR}/darwin/network/slirp
        ${SLIRP_INCLUDE_DIRS}
        ${GLIB_INCLUDE_DIRS})
    target_compile_features(uml_slirp_network PUBLIC cxx_std_20)
    target_compile_options(uml_slirp_network PRIVATE -Wall -Wextra -Werror)
    set_target_properties(uml_slirp_network PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_sources(uml_backend PRIVATE $<TARGET_OBJECTS:uml_slirp_network>)

    target_include_directories(uml_backend PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/loader
        ${SLIRP_INCLUDE_DIRS}
        ${GLIB_INCLUDE_DIRS}
        ${PCRE2_INCLUDE_DIRS})

    set(GETTEXT_STATIC /opt/homebrew/opt/gettext/lib/libintl.a)
    foreach(STATIC_LIBRARY
            ${SLIRP_LIBDIR}/libslirp.a
            ${GLIB_LIBDIR}/libglib-2.0.a
            ${PCRE2_LIBDIR}/libpcre2-8.a
            ${GETTEXT_STATIC})
        if(NOT EXISTS ${STATIC_LIBRARY})
            message(FATAL_ERROR "Required static dependency is missing: ${STATIC_LIBRARY}")
        endif()
    endforeach()

    target_link_libraries(uml_backend PRIVATE
        ${SLIRP_LIBDIR}/libslirp.a
        ${GLIB_LIBDIR}/libglib-2.0.a
        ${PCRE2_LIBDIR}/libpcre2-8.a
        ${GETTEXT_STATIC}
        resolv iconv m
        "-framework Foundation"
        "-framework CoreFoundation")

    if(UML_BUILD_TESTS)
        add_executable(uml_slirp_network_tests
            ${PROJECT_SOURCE_DIR}/darwin/probes/network.cpp
            $<TARGET_OBJECTS:uml_slirp_network>)
        target_include_directories(uml_slirp_network_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_features(uml_slirp_network_tests PRIVATE cxx_std_20)
        target_compile_options(uml_slirp_network_tests PRIVATE
            -Wall -Wextra -Werror)
        target_link_libraries(uml_slirp_network_tests PRIVATE
            ${SLIRP_LIBDIR}/libslirp.a
            ${GLIB_LIBDIR}/libglib-2.0.a
            ${PCRE2_LIBDIR}/libpcre2-8.a
            ${GETTEXT_STATIC}
            resolv iconv m
            "-framework Foundation"
            "-framework CoreFoundation")
        add_test(NAME slirp-network-arp COMMAND uml_slirp_network_tests)
    endif()
endif()

if(UML_BUILD_TESTS)
    add_executable(uml_native_mmu_tests
        tests/native_mmu_tests.cpp
        $<TARGET_OBJECTS:uml_native_mmu>)
    target_include_directories(uml_native_mmu_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/runtime/mmu)
    target_compile_features(uml_native_mmu_tests PRIVATE cxx_std_20)
    target_compile_options(uml_native_mmu_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME native-4k-shadow-mmu COMMAND uml_native_mmu_tests)

    add_executable(uml_tlb_sync_range_tests
        tests/tlb_sync_range_tests.cpp)
    target_include_directories(uml_tlb_sync_range_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/linux-6.18/arch/um/include)
    target_compile_features(uml_tlb_sync_range_tests PRIVATE cxx_std_20)
    target_compile_options(uml_tlb_sync_range_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME uml-tlb-sync-ranges COMMAND uml_tlb_sync_range_tests)

    add_executable(uml_arm64_gadget_tlb_layout_tests
        tests/arm64_gadget_tlb_layout_tests.cpp)
    target_include_directories(uml_arm64_gadget_tlb_layout_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(uml_arm64_gadget_tlb_layout_tests PRIVATE cxx_std_20)
    target_compile_options(uml_arm64_gadget_tlb_layout_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-gadget-tlb-layout
        COMMAND uml_arm64_gadget_tlb_layout_tests)

    add_executable(uml_static_mmu_batch_tests
        tests/static_mmu_batch_tests.cpp
        $<TARGET_OBJECTS:uml_static_mmu>)
    target_include_directories(uml_static_mmu_batch_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_features(uml_static_mmu_batch_tests PRIVATE cxx_std_20)
    target_compile_options(uml_static_mmu_batch_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME static-mmu-batch COMMAND uml_static_mmu_batch_tests)

    add_executable(uml_backend_api_tests tests/backend_api_tests.cpp)
    target_link_libraries(uml_backend_api_tests PRIVATE UML::Backend)
    target_compile_features(uml_backend_api_tests PRIVATE cxx_std_20)
    target_compile_options(uml_backend_api_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME backend-native-entry-api COMMAND uml_backend_api_tests)

    add_executable(uml_arm64_dispatch_tests
        tests/arm64_dispatch_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/arm64-neon-inventory.s
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(uml_arm64_dispatch_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_dispatch_tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME arm64-static-dispatch COMMAND uml_arm64_dispatch_tests)

    add_executable(uml_arm64_static_boundary_state_tests
        tests/arm64_static_boundary_state_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(uml_arm64_static_boundary_state_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_static_boundary_state_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-static-boundary-state
        COMMAND uml_arm64_static_boundary_state_tests)

    add_executable(uml_arm64_static_translation_tests
        tests/arm64_static_translation_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(uml_arm64_static_translation_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_static_translation_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-static-translation
        COMMAND uml_arm64_static_translation_tests)

    add_executable(uml_arm64_gadget_tests
        tests/arm64_gadget_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_gadget_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-static-gadgets COMMAND uml_arm64_gadget_tests)

    add_executable(uml_arm64_gadget_hot_path_tests
		tests/arm64_gadget_hot_path_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_gadget_hot_path_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_hot_path_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_gadget_hot_path_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-gadget-hot-path COMMAND uml_arm64_gadget_hot_path_tests)

    add_executable(uml_arm64_gadget_nzcv_residency_tests
		tests/arm64_gadget_nzcv_residency_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_gadget_nzcv_residency_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_nzcv_residency_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-gadget-nzcv-residency
		COMMAND uml_arm64_gadget_nzcv_residency_tests)

    add_executable(uml_arm64_gadget_fp_environment_tests
		tests/arm64_gadget_fp_environment_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_gadget_fp_environment_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_definitions(uml_arm64_gadget_fp_environment_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    target_compile_options(uml_arm64_gadget_fp_environment_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-gadget-fp-environment
		COMMAND uml_arm64_gadget_fp_environment_tests)

    add_executable(uml_arm64_gadget_link_tests
		tests/arm64_gadget_link_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_gadget_link_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_link_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_gadget_link_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-gadget-links COMMAND uml_arm64_gadget_link_tests)

    add_executable(uml_arm64_gadget_indirect_link_tests
		tests/arm64_gadget_indirect_link_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_gadget_indirect_link_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_indirect_link_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-gadget-indirect-links
		COMMAND uml_arm64_gadget_indirect_link_tests)

    add_executable(uml_arm64_base_arithmetic_tests
		tests/arm64_base_arithmetic_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_arithmetic_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_arithmetic_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-arithmetic
		COMMAND uml_arm64_base_arithmetic_tests)

    add_executable(uml_arm64_base_bit_tests
		tests/arm64_base_bit_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_bit_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_bit_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-bits COMMAND uml_arm64_base_bit_tests)

    add_executable(uml_arm64_base_multiply_tests
		tests/arm64_base_multiply_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_multiply_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_multiply_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-multiply COMMAND uml_arm64_base_multiply_tests)

    add_executable(uml_arm64_base_condition_tests
		tests/arm64_base_condition_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_condition_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_condition_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-conditions COMMAND uml_arm64_base_condition_tests)

    add_executable(uml_arm64_base_add_sub_tests
		tests/arm64_base_add_sub_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_add_sub_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_add_sub_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-add-sub COMMAND uml_arm64_base_add_sub_tests)

    add_executable(uml_arm64_base_mte_pointer_tests
		tests/arm64_base_mte_pointer_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_mte_pointer_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mte_pointer_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_mte_pointer_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-mte-pointer
		COMMAND uml_arm64_base_mte_pointer_tests)

    add_executable(uml_arm64_base_mte_tag_tests
		tests/arm64_base_mte_tag_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_mte_tag_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mte_tag_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_mte_tag_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-mte-tag COMMAND uml_arm64_base_mte_tag_tests)

    add_executable(uml_arm64_base_mte_random_tag_tests
		tests/arm64_base_mte_random_tag_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_mte_random_tag_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mte_random_tag_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_mte_random_tag_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-mte-random-tag
		COMMAND uml_arm64_base_mte_random_tag_tests)

    add_executable(uml_arm64_base_cpa_pointer_tests
		tests/arm64_base_cpa_pointer_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_cpa_pointer_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_cpa_pointer_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_cpa_pointer_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-cpa-pointer
		COMMAND uml_arm64_base_cpa_pointer_tests)

    add_executable(uml_arm64_base_cpa_multiply_tests
		tests/arm64_base_cpa_multiply_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_cpa_multiply_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_cpa_multiply_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_cpa_multiply_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-cpa-multiply
		COMMAND uml_arm64_base_cpa_multiply_tests)

    add_executable(uml_arm64_base_cmpbr_immediate_tests
		tests/arm64_base_cmpbr_immediate_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_cmpbr_immediate_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_cmpbr_immediate_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_cmpbr_immediate_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-cmpbr-immediate
		COMMAND uml_arm64_base_cmpbr_immediate_tests)

    add_executable(uml_arm64_base_cmpbr_register_tests
		tests/arm64_base_cmpbr_register_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_cmpbr_register_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_cmpbr_register_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_cmpbr_register_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-cmpbr-register
		COMMAND uml_arm64_base_cmpbr_register_tests)

    add_executable(uml_arm64_base_logical_tests
		tests/arm64_base_logical_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_logical_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_logical_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-logical COMMAND uml_arm64_base_logical_tests)

    add_executable(uml_arm64_base_move_tests
		tests/arm64_base_move_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_move_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_move_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-move-wide COMMAND uml_arm64_base_move_tests)

    add_executable(uml_arm64_base_bitfield_tests
		tests/arm64_base_bitfield_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_bitfield_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_bitfield_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-bitfield COMMAND uml_arm64_base_bitfield_tests)

    add_executable(uml_arm64_base_extract_tests
		tests/arm64_base_extract_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_extract_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_extract_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-extract COMMAND uml_arm64_base_extract_tests)

    add_executable(uml_arm64_base_control_tests
		tests/arm64_base_control_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_control_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_control_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-control COMMAND uml_arm64_base_control_tests)

    add_executable(uml_arm64_base_crc_tests
		tests/arm64_base_crc_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_crc_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_crc_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-crc COMMAND uml_arm64_base_crc_tests)

    add_executable(uml_arm64_base_barrier_tests
		tests/arm64_base_barrier_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_barrier_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_barrier_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-barriers COMMAND uml_arm64_base_barrier_tests)

    add_executable(uml_arm64_base_hint_tests
		tests/arm64_base_hint_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_hint_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_hint_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_hint_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-hints COMMAND uml_arm64_base_hint_tests)

    add_executable(uml_arm64_base_system_register_tests
		tests/arm64_base_system_register_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_system_register_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_system_register_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_system_register_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-system-registers
		COMMAND uml_arm64_base_system_register_tests)

    add_executable(uml_arm64_base_cache_zero_tests
		tests/arm64_base_cache_zero_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_cache_zero_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_cache_zero_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_cache_zero_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-cache-zero
		COMMAND uml_arm64_base_cache_zero_tests)

	foreach(SYSTEM_TEST IN ITEMS
			system_register_allocation
			system_instruction
			system_alias
			gcs_system)
		add_executable(uml_arm64_base_${SYSTEM_TEST}_tests
			tests/arm64_base_${SYSTEM_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(uml_arm64_base_${SYSTEM_TEST}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_base_${SYSTEM_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_compile_definitions(uml_arm64_base_${SYSTEM_TEST}_tests PRIVATE
			UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
		add_test(NAME arm64-base-${SYSTEM_TEST}
			COMMAND uml_arm64_base_${SYSTEM_TEST}_tests)
	endforeach()

    add_executable(uml_arm64_base_pstate_tests
		tests/arm64_base_pstate_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_pstate_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pstate_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_pstate_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-pstate COMMAND uml_arm64_base_pstate_tests)

    add_executable(uml_arm64_base_pauth_strip_tests
		tests/arm64_base_pauth_strip_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_pauth_strip_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_strip_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_pauth_strip_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-pauth-strip
		COMMAND uml_arm64_base_pauth_strip_tests)

    add_executable(uml_arm64_base_pauth_code_tests
		tests/arm64_base_pauth_code_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_pauth_code_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_code_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_pauth_code_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-pauth-code
		COMMAND uml_arm64_base_pauth_code_tests)

    add_executable(uml_arm64_base_pauth_auth_tests
		tests/arm64_base_pauth_auth_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_pauth_auth_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_auth_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_pauth_auth_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-pauth-auth
		COMMAND uml_arm64_base_pauth_auth_tests)

    add_executable(uml_arm64_base_pauth_generic_tests
		tests/arm64_base_pauth_generic_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_pauth_generic_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_generic_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_pauth_generic_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-pauth-generic
		COMMAND uml_arm64_base_pauth_generic_tests)

    add_executable(uml_arm64_base_pauth_branch_tests
		tests/arm64_base_pauth_branch_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_pauth_branch_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_branch_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_pauth_branch_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-pauth-branch
		COMMAND uml_arm64_base_pauth_branch_tests)

    add_executable(uml_arm64_base_pauth_return_tests
		tests/arm64_base_pauth_return_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_pauth_return_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_return_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_pauth_return_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-pauth-return
		COMMAND uml_arm64_base_pauth_return_tests)

    add_executable(uml_arm64_base_pauth_load_tests
		tests/arm64_base_pauth_load_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_pauth_load_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_load_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_pauth_load_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-pauth-load
		COMMAND uml_arm64_base_pauth_load_tests)

    add_executable(uml_arm64_base_pauth_lr_data_tests
		tests/arm64_base_pauth_lr_data_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_pauth_lr_data_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_lr_data_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_pauth_lr_data_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-pauth-lr-data
		COMMAND uml_arm64_base_pauth_lr_data_tests)

    add_executable(uml_arm64_base_pauth_lr_return_tests
		tests/arm64_base_pauth_lr_return_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_pauth_lr_return_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_lr_return_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_pauth_lr_return_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-pauth-lr-return
		COMMAND uml_arm64_base_pauth_lr_return_tests)

    add_executable(uml_arm64_base_exception_tests
		tests/arm64_base_exception_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_exception_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_exception_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-exceptions COMMAND uml_arm64_base_exception_tests)

    add_executable(uml_arm64_base_prefetch_tests
		tests/arm64_base_prefetch_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_prefetch_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_prefetch_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-prefetch COMMAND uml_arm64_base_prefetch_tests)

    add_executable(uml_arm64_base_range_prefetch_tests
		tests/arm64_base_range_prefetch_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_range_prefetch_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_range_prefetch_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_range_prefetch_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-range-prefetch
		COMMAND uml_arm64_base_range_prefetch_tests)

    add_executable(uml_arm64_base_ls64_load_tests
		tests/arm64_base_ls64_load_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_ls64_load_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_ls64_load_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_ls64_load_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-ls64-load
		COMMAND uml_arm64_base_ls64_load_tests)

    add_executable(uml_arm64_base_ls64_store_tests
		tests/arm64_base_ls64_store_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_ls64_store_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_ls64_store_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_ls64_store_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-ls64-store
		COMMAND uml_arm64_base_ls64_store_tests)

    add_executable(uml_arm64_base_mops_copy_forward_tests
		tests/arm64_base_mops_copy_forward_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_mops_copy_forward_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mops_copy_forward_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_mops_copy_forward_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-mops-copy-forward
		COMMAND uml_arm64_base_mops_copy_forward_tests)

    add_executable(uml_arm64_base_mops_copy_tests
		tests/arm64_base_mops_copy_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_mops_copy_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mops_copy_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_mops_copy_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-mops-copy
		COMMAND uml_arm64_base_mops_copy_tests)

    add_executable(uml_arm64_base_mops_set_tests
		tests/arm64_base_mops_set_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_mops_set_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mops_set_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_mops_set_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-mops-set
		COMMAND uml_arm64_base_mops_set_tests)

    add_executable(uml_arm64_base_mops_set_tag_tests
		tests/arm64_base_mops_set_tag_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_mops_set_tag_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mops_set_tag_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_mops_set_tag_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-mops-set-tag
		COMMAND uml_arm64_base_mops_set_tag_tests)

    add_executable(uml_arm64_base_mops_set_tag_fault_tests
		tests/arm64_base_mops_set_tag_fault_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_mops_set_tag_fault_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mops_set_tag_fault_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_mops_set_tag_fault_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-mops-set-tag-fault
		COMMAND uml_arm64_base_mops_set_tag_fault_tests)

    add_executable(uml_arm64_base_mte_tag_load_tests
		tests/arm64_base_mte_tag_load_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_mte_tag_load_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mte_tag_load_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_mte_tag_load_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-mte-tag-load
		COMMAND uml_arm64_base_mte_tag_load_tests)

    add_executable(uml_arm64_base_mte_tag_store_tests
		tests/arm64_base_mte_tag_store_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_mte_tag_store_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mte_tag_store_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_mte_tag_store_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-mte-tag-store
		COMMAND uml_arm64_base_mte_tag_store_tests)

    add_executable(uml_arm64_base_mte_tag_data_tests
		tests/arm64_base_mte_tag_data_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_mte_tag_data_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_mte_tag_data_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_mte_tag_data_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-mte-tag-data
		COMMAND uml_arm64_base_mte_tag_data_tests)

    add_executable(uml_arm64_literal_load_tests
		tests/arm64_literal_load_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_literal_load_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_literal_load_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-literal-loads COMMAND uml_arm64_literal_load_tests)

    add_executable(uml_arm64_base_load_store_immediate_tests
		tests/arm64_base_load_store_immediate_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_load_store_immediate_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_load_store_immediate_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-load-store-immediate
		COMMAND uml_arm64_base_load_store_immediate_tests)

    add_executable(uml_arm64_base_load_store_register_tests
		tests/arm64_base_load_store_register_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_load_store_register_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_load_store_register_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-load-store-register
		COMMAND uml_arm64_base_load_store_register_tests)

    add_executable(uml_arm64_base_load_store_pair_tests
		tests/arm64_base_load_store_pair_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_load_store_pair_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_load_store_pair_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-load-store-pair
		COMMAND uml_arm64_base_load_store_pair_tests)

    add_executable(uml_arm64_base_ordered_tests
		tests/arm64_base_ordered_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_ordered_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_ordered_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-ordered COMMAND uml_arm64_base_ordered_tests)

    add_executable(uml_arm64_base_exclusive_tests
		tests/arm64_base_exclusive_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_exclusive_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_exclusive_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-exclusive COMMAND uml_arm64_base_exclusive_tests)

    add_executable(uml_arm64_base_exclusive_pair_tests
		tests/arm64_base_exclusive_pair_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_exclusive_pair_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_exclusive_pair_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-exclusive-pair
		COMMAND uml_arm64_base_exclusive_pair_tests)

    add_executable(uml_arm64_base_compare_swap_tests
		tests/arm64_base_compare_swap_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_compare_swap_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_compare_swap_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-compare-swap
		COMMAND uml_arm64_base_compare_swap_tests)

    add_executable(uml_arm64_base_compare_swap_pair_tests
		tests/arm64_base_compare_swap_pair_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_compare_swap_pair_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_compare_swap_pair_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-compare-swap-pair
		COMMAND uml_arm64_base_compare_swap_pair_tests)

    add_executable(uml_arm64_base_atomic_bit_tests
		tests/arm64_base_atomic_bit_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_atomic_bit_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_atomic_bit_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-atomic-bit
		COMMAND uml_arm64_base_atomic_bit_tests)

    add_executable(uml_arm64_base_atomic_minmax_tests
		tests/arm64_base_atomic_minmax_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_atomic_minmax_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_atomic_minmax_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-atomic-minmax
		COMMAND uml_arm64_base_atomic_minmax_tests)

    add_executable(uml_arm64_base_atomic_swap_tests
		tests/arm64_base_atomic_swap_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_atomic_swap_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_atomic_swap_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-atomic-swap
		COMMAND uml_arm64_base_atomic_swap_tests)

    add_executable(uml_arm64_base_atomic_pair128_tests
		tests/arm64_base_atomic_pair128_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_atomic_pair128_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_atomic_pair128_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-atomic-pair128
		COMMAND uml_arm64_base_atomic_pair128_tests)

    foreach(atomic_floating_suite IN ITEMS allocation bfloat precision
			ordering_alias fault gadget)
	add_executable(uml_arm64_atomic_floating_${atomic_floating_suite}_tests
		tests/arm64_atomic_floating_${atomic_floating_suite}_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
		uml_arm64_atomic_floating_${atomic_floating_suite}_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_atomic_floating_${atomic_floating_suite}_tests PRIVATE
		-Wall -Wextra -Werror)
	add_test(NAME arm64-atomic-floating-${atomic_floating_suite}
		COMMAND uml_arm64_atomic_floating_${atomic_floating_suite}_tests)
    endforeach()

    add_executable(uml_arm64_base_rcw_scalar_tests
		tests/arm64_base_rcw_scalar_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_rcw_scalar_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_rcw_scalar_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_rcw_scalar_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-rcw-scalar
		COMMAND uml_arm64_base_rcw_scalar_tests)

    add_executable(uml_arm64_base_rcw_pair_tests
		tests/arm64_base_rcw_pair_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_rcw_pair_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_rcw_pair_tests PRIVATE
		-Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_base_rcw_pair_tests PRIVATE
		UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
    add_test(NAME arm64-base-rcw-pair
		COMMAND uml_arm64_base_rcw_pair_tests)

    add_executable(uml_arm64_base_clear_exclusive_tests
		tests/arm64_base_clear_exclusive_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_clear_exclusive_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_clear_exclusive_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-clear-exclusive
		COMMAND uml_arm64_base_clear_exclusive_tests)

    add_executable(uml_arm64_base_rcpc_no_offset_tests
		tests/arm64_base_rcpc_no_offset_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_rcpc_no_offset_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_rcpc_no_offset_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-rcpc-no-offset
		COMMAND uml_arm64_base_rcpc_no_offset_tests)

    add_executable(uml_arm64_base_rcpc_unscaled_tests
		tests/arm64_base_rcpc_unscaled_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_rcpc_unscaled_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_rcpc_unscaled_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-rcpc-unscaled
		COMMAND uml_arm64_base_rcpc_unscaled_tests)

    add_executable(uml_arm64_base_ordered_pair_tests
		tests/arm64_base_ordered_pair_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_ordered_pair_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_ordered_pair_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-ordered-pair
		COMMAND uml_arm64_base_ordered_pair_tests)

    add_executable(uml_arm64_base_consistent_pair_tests
		tests/arm64_base_consistent_pair_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_consistent_pair_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_consistent_pair_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-base-consistent-pair
		COMMAND uml_arm64_base_consistent_pair_tests)

    add_executable(uml_arm64_simd_load_store_tests
		tests/arm64_simd_load_store_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_simd_load_store_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_simd_load_store_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-simd-load-store
		COMMAND uml_arm64_simd_load_store_tests)

    add_executable(uml_arm64_simd_load_store_pair_tests
		tests/arm64_simd_load_store_pair_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_simd_load_store_pair_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_simd_load_store_pair_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-simd-load-store-pair
		COMMAND uml_arm64_simd_load_store_pair_tests)

    add_executable(uml_arm64_simd_multiple_structure_tests
		tests/arm64_simd_multiple_structure_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_simd_multiple_structure_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_simd_multiple_structure_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-simd-multiple-structure
		COMMAND uml_arm64_simd_multiple_structure_tests)

    add_executable(uml_arm64_simd_single_structure_tests
		tests/arm64_simd_single_structure_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_simd_single_structure_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_simd_single_structure_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-simd-single-structure
		COMMAND uml_arm64_simd_single_structure_tests)

    add_executable(uml_arm64_simd_replicate_structure_tests
		tests/arm64_simd_replicate_structure_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_simd_replicate_structure_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_simd_replicate_structure_tests PRIVATE
		-Wall -Wextra -Werror)
    add_test(NAME arm64-simd-replicate-structure
		COMMAND uml_arm64_simd_replicate_structure_tests)

    foreach(simd_ordered_suite IN ITEMS allocation behavior gadget)
		add_executable(uml_arm64_simd_ordered_${simd_ordered_suite}_tests
			tests/arm64_simd_ordered_${simd_ordered_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_ordered_${simd_ordered_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_ordered_${simd_ordered_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-ordered-${simd_ordered_suite}
			COMMAND uml_arm64_simd_ordered_${simd_ordered_suite}_tests)
	endforeach()

	add_executable(uml_arm64_simd_baseline_inventory_tests
		tests/arm64_simd_baseline_inventory_tests.cpp
		${PROJECT_SOURCE_DIR}/tests/arm64-neon-inventory.s
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(uml_arm64_simd_baseline_inventory_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(uml_arm64_simd_baseline_inventory_tests PRIVATE
		-Wall -Wextra -Werror)
	add_test(NAME arm64-simd-baseline-inventory
		COMMAND uml_arm64_simd_baseline_inventory_tests)

	foreach(simd_three_same_suite IN ITEMS allocation arithmetic saturation)
		add_executable(uml_arm64_simd_three_same_${simd_three_same_suite}_tests
			tests/arm64_simd_three_same_${simd_three_same_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_three_same_${simd_three_same_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_three_same_${simd_three_same_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-three-same-${simd_three_same_suite}
			COMMAND uml_arm64_simd_three_same_${simd_three_same_suite}_tests)
	endforeach()

	foreach(simd_widening_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_widening_${simd_widening_suite}_tests
			tests/arm64_simd_widening_${simd_widening_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_widening_${simd_widening_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_widening_${simd_widening_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-widening-${simd_widening_suite}
			COMMAND uml_arm64_simd_widening_${simd_widening_suite}_tests)
	endforeach()

	foreach(simd_two_register_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_two_register_${simd_two_register_suite}_tests
			tests/arm64_simd_two_register_${simd_two_register_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_two_register_${simd_two_register_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_two_register_${simd_two_register_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-two-register-${simd_two_register_suite}
			COMMAND uml_arm64_simd_two_register_${simd_two_register_suite}_tests)
	endforeach()

	foreach(simd_reduce_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_reduce_${simd_reduce_suite}_tests
			tests/arm64_simd_reduce_${simd_reduce_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_reduce_${simd_reduce_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_reduce_${simd_reduce_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-reduce-${simd_reduce_suite}
			COMMAND uml_arm64_simd_reduce_${simd_reduce_suite}_tests)
	endforeach()

	foreach(simd_estimate_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_estimate_${simd_estimate_suite}_tests
			tests/arm64_simd_estimate_${simd_estimate_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_estimate_${simd_estimate_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_estimate_${simd_estimate_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-estimate-${simd_estimate_suite}
			COMMAND uml_arm64_simd_estimate_${simd_estimate_suite}_tests)
	endforeach()

	foreach(simd_bitwise_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_bitwise_${simd_bitwise_suite}_tests
			tests/arm64_simd_bitwise_${simd_bitwise_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_bitwise_${simd_bitwise_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_bitwise_${simd_bitwise_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-bitwise-${simd_bitwise_suite}
			COMMAND uml_arm64_simd_bitwise_${simd_bitwise_suite}_tests)
	endforeach()

	foreach(simd_immediate_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_immediate_${simd_immediate_suite}_tests
			tests/arm64_simd_immediate_${simd_immediate_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_immediate_${simd_immediate_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_immediate_${simd_immediate_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-immediate-${simd_immediate_suite}
			COMMAND uml_arm64_simd_immediate_${simd_immediate_suite}_tests)
	endforeach()

	foreach(simd_rearrange_suite IN ITEMS allocation permute_behavior table_behavior)
		add_executable(uml_arm64_simd_rearrange_${simd_rearrange_suite}_tests
			tests/arm64_simd_rearrange_${simd_rearrange_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_rearrange_${simd_rearrange_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_rearrange_${simd_rearrange_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-rearrange-${simd_rearrange_suite}
			COMMAND uml_arm64_simd_rearrange_${simd_rearrange_suite}_tests)
	endforeach()

	foreach(simd_lut_suite IN ITEMS allocation behavior gadget)
		add_executable(uml_arm64_simd_lut_${simd_lut_suite}_tests
			tests/arm64_simd_lut_${simd_lut_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_lut_${simd_lut_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_lut_${simd_lut_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-lut-${simd_lut_suite}
			COMMAND uml_arm64_simd_lut_${simd_lut_suite}_tests)
	endforeach()

	foreach(basic_floating_suite IN ITEMS allocation behavior control gadget)
		add_executable(uml_arm64_basic_floating_${basic_floating_suite}_tests
			tests/arm64_basic_floating_${basic_floating_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_basic_floating_${basic_floating_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_basic_floating_${basic_floating_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-basic-floating-${basic_floating_suite}
			COMMAND uml_arm64_basic_floating_${basic_floating_suite}_tests)
	endforeach()

	foreach(fused_floating_suite IN ITEMS allocation behavior control gadget)
		add_executable(uml_arm64_fused_floating_${fused_floating_suite}_tests
			tests/arm64_fused_floating_${fused_floating_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_fused_floating_${fused_floating_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_fused_floating_${fused_floating_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-fused-floating-${fused_floating_suite}
			COMMAND uml_arm64_fused_floating_${fused_floating_suite}_tests)
	endforeach()

	foreach(floating_round_suite IN ITEMS allocation behavior control gadget)
		add_executable(uml_arm64_floating_round_${floating_round_suite}_tests
			tests/arm64_floating_round_${floating_round_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_floating_round_${floating_round_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_floating_round_${floating_round_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-floating-round-${floating_round_suite}
			COMMAND uml_arm64_floating_round_${floating_round_suite}_tests)
	endforeach()

	foreach(floating_horizontal_suite IN ITEMS allocation binary pairwise
			reduction control gadget)
		add_executable(
			uml_arm64_floating_horizontal_${floating_horizontal_suite}_tests
			tests/arm64_floating_horizontal_${floating_horizontal_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_floating_horizontal_${floating_horizontal_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_floating_horizontal_${floating_horizontal_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-floating-horizontal-${floating_horizontal_suite}
			COMMAND
			uml_arm64_floating_horizontal_${floating_horizontal_suite}_tests)
	endforeach()

	foreach(floating_reciprocal_suite IN ITEMS allocation behavior control gadget)
		add_executable(
			uml_arm64_floating_reciprocal_${floating_reciprocal_suite}_tests
			tests/arm64_floating_reciprocal_${floating_reciprocal_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_floating_reciprocal_${floating_reciprocal_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_floating_reciprocal_${floating_reciprocal_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-floating-reciprocal-${floating_reciprocal_suite}
			COMMAND
			uml_arm64_floating_reciprocal_${floating_reciprocal_suite}_tests)
	endforeach()

	foreach(floating_complex_suite IN ITEMS allocation add multiply indexed
			control gadget)
		add_executable(
			uml_arm64_floating_complex_${floating_complex_suite}_tests
			tests/arm64_floating_complex_${floating_complex_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_floating_complex_${floating_complex_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_floating_complex_${floating_complex_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-floating-complex-${floating_complex_suite}
			COMMAND
			uml_arm64_floating_complex_${floating_complex_suite}_tests)
	endforeach()

	foreach(floating_absolute_minmax_suite IN ITEMS allocation behavior control
			gadget)
		add_executable(
			uml_arm64_floating_absolute_minmax_${floating_absolute_minmax_suite}_tests
			tests/arm64_floating_absolute_minmax_${floating_absolute_minmax_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_floating_absolute_minmax_${floating_absolute_minmax_suite}_tests
			PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_floating_absolute_minmax_${floating_absolute_minmax_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		add_test(
			NAME arm64-floating-absolute-minmax-${floating_absolute_minmax_suite}
			COMMAND
			uml_arm64_floating_absolute_minmax_${floating_absolute_minmax_suite}_tests)
	endforeach()

	foreach(floating_scale_suite IN ITEMS allocation behavior control gadget)
		add_executable(uml_arm64_floating_scale_${floating_scale_suite}_tests
			tests/arm64_floating_scale_${floating_scale_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_floating_scale_${floating_scale_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_floating_scale_${floating_scale_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-floating-scale-${floating_scale_suite}
			COMMAND uml_arm64_floating_scale_${floating_scale_suite}_tests)
	endforeach()

	foreach(bfloat_conversion_suite IN ITEMS allocation behavior control gadget)
		add_executable(
			uml_arm64_bfloat_conversion_${bfloat_conversion_suite}_tests
			tests/arm64_bfloat_conversion_${bfloat_conversion_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_bfloat_conversion_${bfloat_conversion_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_bfloat_conversion_${bfloat_conversion_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-bfloat-conversion-${bfloat_conversion_suite}
			COMMAND
			uml_arm64_bfloat_conversion_${bfloat_conversion_suite}_tests)
	endforeach()

	foreach(bfloat_multiply_suite IN ITEMS allocation vector indexed control
			gadget)
		add_executable(
			uml_arm64_bfloat_multiply_${bfloat_multiply_suite}_tests
			tests/arm64_bfloat_multiply_${bfloat_multiply_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_bfloat_multiply_${bfloat_multiply_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_bfloat_multiply_${bfloat_multiply_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-bfloat-multiply-${bfloat_multiply_suite}
			COMMAND
			uml_arm64_bfloat_multiply_${bfloat_multiply_suite}_tests)
	endforeach()

	foreach(bfloat_dot_suite IN ITEMS allocation vector indexed control gadget)
		add_executable(uml_arm64_bfloat_dot_${bfloat_dot_suite}_tests
			tests/arm64_bfloat_dot_${bfloat_dot_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(uml_arm64_bfloat_dot_${bfloat_dot_suite}_tests
			PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_bfloat_dot_${bfloat_dot_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		add_test(NAME arm64-bfloat-dot-${bfloat_dot_suite}
			COMMAND uml_arm64_bfloat_dot_${bfloat_dot_suite}_tests)
	endforeach()

	foreach(bfloat_matrix_suite IN ITEMS allocation behavior control gadget)
		add_executable(uml_arm64_bfloat_matrix_${bfloat_matrix_suite}_tests
			tests/arm64_bfloat_matrix_${bfloat_matrix_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_bfloat_matrix_${bfloat_matrix_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_bfloat_matrix_${bfloat_matrix_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-bfloat-matrix-${bfloat_matrix_suite}
			COMMAND uml_arm64_bfloat_matrix_${bfloat_matrix_suite}_tests)
	endforeach()

	foreach(half_fused_long_suite IN ITEMS allocation vector indexed control
			gadget)
		add_executable(uml_arm64_half_fused_long_${half_fused_long_suite}_tests
			tests/arm64_half_fused_long_${half_fused_long_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_half_fused_long_${half_fused_long_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_half_fused_long_${half_fused_long_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-half-fused-long-${half_fused_long_suite}
			COMMAND uml_arm64_half_fused_long_${half_fused_long_suite}_tests)
	endforeach()

	foreach(half_matrix_suite IN ITEMS allocation non_widening widening control
			gadget)
		add_executable(uml_arm64_half_matrix_${half_matrix_suite}_tests
			tests/arm64_half_matrix_${half_matrix_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_half_matrix_${half_matrix_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_half_matrix_${half_matrix_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-half-matrix-${half_matrix_suite}
			COMMAND uml_arm64_half_matrix_${half_matrix_suite}_tests)
	endforeach()

	foreach(half_dot_suite IN ITEMS allocation vector indexed control gadget)
		add_executable(uml_arm64_half_dot_${half_dot_suite}_tests
			tests/arm64_half_dot_${half_dot_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(uml_arm64_half_dot_${half_dot_suite}_tests
			PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_half_dot_${half_dot_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		add_test(NAME arm64-half-dot-${half_dot_suite}
			COMMAND uml_arm64_half_dot_${half_dot_suite}_tests)
	endforeach()

	foreach(simd_extract_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_extract_${simd_extract_suite}_tests
			tests/arm64_simd_extract_${simd_extract_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_extract_${simd_extract_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_extract_${simd_extract_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-extract-${simd_extract_suite}
			COMMAND uml_arm64_simd_extract_${simd_extract_suite}_tests)
	endforeach()

	foreach(simd_aes_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_aes_${simd_aes_suite}_tests
			tests/arm64_simd_aes_${simd_aes_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_aes_${simd_aes_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_aes_${simd_aes_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-aes-${simd_aes_suite}
			COMMAND uml_arm64_simd_aes_${simd_aes_suite}_tests)
	endforeach()

	foreach(simd_sha3_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_sha3_${simd_sha3_suite}_tests
			tests/arm64_simd_sha3_${simd_sha3_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_sha3_${simd_sha3_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_sha3_${simd_sha3_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-sha3-${simd_sha3_suite}
			COMMAND uml_arm64_simd_sha3_${simd_sha3_suite}_tests)
	endforeach()

	foreach(simd_pmull_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_pmull_${simd_pmull_suite}_tests
			tests/arm64_simd_pmull_${simd_pmull_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_pmull_${simd_pmull_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_pmull_${simd_pmull_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-pmull-${simd_pmull_suite}
			COMMAND uml_arm64_simd_pmull_${simd_pmull_suite}_tests)
	endforeach()

	foreach(simd_addp_scalar_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_addp_scalar_${simd_addp_scalar_suite}_tests
			tests/arm64_simd_addp_scalar_${simd_addp_scalar_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_addp_scalar_${simd_addp_scalar_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_addp_scalar_${simd_addp_scalar_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-addp-scalar-${simd_addp_scalar_suite}
			COMMAND uml_arm64_simd_addp_scalar_${simd_addp_scalar_suite}_tests)
	endforeach()

	foreach(simd_lane_move_suite IN ITEMS allocation duplicate insert gpr)
		add_executable(uml_arm64_simd_lane_move_${simd_lane_move_suite}_tests
			tests/arm64_simd_lane_move_${simd_lane_move_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_lane_move_${simd_lane_move_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_lane_move_${simd_lane_move_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-lane-move-${simd_lane_move_suite}
			COMMAND uml_arm64_simd_lane_move_${simd_lane_move_suite}_tests)
	endforeach()

	foreach(simd_shift_immediate_suite IN ITEMS allocation right left narrow)
		add_executable(
			uml_arm64_simd_shift_immediate_${simd_shift_immediate_suite}_tests
			tests/arm64_simd_shift_immediate_${simd_shift_immediate_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_shift_immediate_${simd_shift_immediate_suite}_tests
			PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_shift_immediate_${simd_shift_immediate_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		add_test(NAME arm64-simd-shift-immediate-${simd_shift_immediate_suite}
			COMMAND
			uml_arm64_simd_shift_immediate_${simd_shift_immediate_suite}_tests)
	endforeach()

	foreach(simd_shift_left_long_suite IN ITEMS allocation behavior)
		add_executable(
			uml_arm64_simd_shift_left_long_${simd_shift_left_long_suite}_tests
			tests/arm64_simd_shift_left_long_${simd_shift_left_long_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_shift_left_long_${simd_shift_left_long_suite}_tests
			PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_shift_left_long_${simd_shift_left_long_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		add_test(NAME arm64-simd-shift-left-long-${simd_shift_left_long_suite}
			COMMAND
			uml_arm64_simd_shift_left_long_${simd_shift_left_long_suite}_tests)
	endforeach()

	foreach(simd_saturating_shift_suite IN ITEMS allocation left narrow)
		add_executable(
			uml_arm64_simd_saturating_shift_${simd_saturating_shift_suite}_tests
			tests/arm64_simd_saturating_shift_${simd_saturating_shift_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_saturating_shift_${simd_saturating_shift_suite}_tests
			PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_saturating_shift_${simd_saturating_shift_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		add_test(NAME arm64-simd-saturating-shift-${simd_saturating_shift_suite}
			COMMAND
			uml_arm64_simd_saturating_shift_${simd_saturating_shift_suite}_tests)
	endforeach()

	foreach(simd_indexed_suite IN ITEMS allocation same long saturation)
		add_executable(uml_arm64_simd_indexed_${simd_indexed_suite}_tests
			tests/arm64_simd_indexed_${simd_indexed_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_indexed_${simd_indexed_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_indexed_${simd_indexed_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-indexed-${simd_indexed_suite}
			COMMAND uml_arm64_simd_indexed_${simd_indexed_suite}_tests)
	endforeach()

	foreach(simd_rdm_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_rdm_${simd_rdm_suite}_tests
			tests/arm64_simd_rdm_${simd_rdm_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_rdm_${simd_rdm_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_rdm_${simd_rdm_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-rdm-${simd_rdm_suite}
			COMMAND uml_arm64_simd_rdm_${simd_rdm_suite}_tests)
	endforeach()

	foreach(simd_dot_suite IN ITEMS allocation vector indexed)
		add_executable(uml_arm64_simd_dot_${simd_dot_suite}_tests
			tests/arm64_simd_dot_${simd_dot_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_dot_${simd_dot_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_dot_${simd_dot_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-dot-${simd_dot_suite}
			COMMAND uml_arm64_simd_dot_${simd_dot_suite}_tests)
	endforeach()

	foreach(simd_fp_indexed_suite IN ITEMS allocation arithmetic extended)
		add_executable(
			uml_arm64_simd_fp_indexed_${simd_fp_indexed_suite}_tests
			tests/arm64_simd_fp_indexed_${simd_fp_indexed_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fp_indexed_${simd_fp_indexed_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_fp_indexed_${simd_fp_indexed_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-fp-indexed-${simd_fp_indexed_suite}
			COMMAND
			uml_arm64_simd_fp_indexed_${simd_fp_indexed_suite}_tests)
	endforeach()

	foreach(simd_matrix_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_matrix_${simd_matrix_suite}_tests
			tests/arm64_simd_matrix_${simd_matrix_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_matrix_${simd_matrix_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_matrix_${simd_matrix_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-matrix-${simd_matrix_suite}
			COMMAND uml_arm64_simd_matrix_${simd_matrix_suite}_tests)
	endforeach()

	foreach(simd_fmulx_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_fmulx_${simd_fmulx_suite}_tests
			tests/arm64_simd_fmulx_${simd_fmulx_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fmulx_${simd_fmulx_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_fmulx_${simd_fmulx_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-fmulx-${simd_fmulx_suite}
			COMMAND uml_arm64_simd_fmulx_${simd_fmulx_suite}_tests)
	endforeach()

	foreach(simd_fabs_fneg_suite IN ITEMS allocation scalar vector)
		add_executable(
			uml_arm64_simd_fabs_fneg_${simd_fabs_fneg_suite}_tests
			tests/arm64_simd_fabs_fneg_${simd_fabs_fneg_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fabs_fneg_${simd_fabs_fneg_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_fabs_fneg_${simd_fabs_fneg_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-fabs-fneg-${simd_fabs_fneg_suite}
			COMMAND
			uml_arm64_simd_fabs_fneg_${simd_fabs_fneg_suite}_tests)
	endforeach()

	foreach(simd_fmov_suite IN ITEMS allocation scalar general)
		add_executable(uml_arm64_simd_fmov_${simd_fmov_suite}_tests
			tests/arm64_simd_fmov_${simd_fmov_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fmov_${simd_fmov_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_fmov_${simd_fmov_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-fmov-${simd_fmov_suite}
			COMMAND uml_arm64_simd_fmov_${simd_fmov_suite}_tests)
	endforeach()

	foreach(simd_fcsel_suite IN ITEMS allocation behavior)
		add_executable(uml_arm64_simd_fcsel_${simd_fcsel_suite}_tests
			tests/arm64_simd_fcsel_${simd_fcsel_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fcsel_${simd_fcsel_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_fcsel_${simd_fcsel_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-fcsel-${simd_fcsel_suite}
			COMMAND uml_arm64_simd_fcsel_${simd_fcsel_suite}_tests)
	endforeach()

	foreach(simd_fcmp_suite IN ITEMS allocation direct conditional fpcr)
		add_executable(uml_arm64_simd_fcmp_${simd_fcmp_suite}_tests
			tests/arm64_simd_fcmp_${simd_fcmp_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fcmp_${simd_fcmp_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_fcmp_${simd_fcmp_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-fcmp-${simd_fcmp_suite}
			COMMAND uml_arm64_simd_fcmp_${simd_fcmp_suite}_tests)
	endforeach()

	foreach(simd_fcvt_precision_suite IN ITEMS allocation exact rounding control)
		add_executable(
			uml_arm64_simd_fcvt_precision_${simd_fcvt_precision_suite}_tests
			tests/arm64_simd_fcvt_precision_${simd_fcvt_precision_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fcvt_precision_${simd_fcvt_precision_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_fcvt_precision_${simd_fcvt_precision_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-fcvt-precision-${simd_fcvt_precision_suite}
			COMMAND
			uml_arm64_simd_fcvt_precision_${simd_fcvt_precision_suite}_tests)
	endforeach()

	foreach(simd_floating_compare_suite IN ITEMS allocation register zero fpcr)
		add_executable(
			uml_arm64_simd_floating_compare_${simd_floating_compare_suite}_tests
			tests/arm64_simd_floating_compare_${simd_floating_compare_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_floating_compare_${simd_floating_compare_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_floating_compare_${simd_floating_compare_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-floating-compare-${simd_floating_compare_suite}
			COMMAND
			uml_arm64_simd_floating_compare_${simd_floating_compare_suite}_tests)
	endforeach()

	foreach(simd_facmp_suite IN ITEMS allocation behavior nan fpcr)
		add_executable(uml_arm64_simd_facmp_${simd_facmp_suite}_tests
			tests/arm64_simd_facmp_${simd_facmp_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_facmp_${simd_facmp_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_simd_facmp_${simd_facmp_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		add_test(NAME arm64-simd-facmp-${simd_facmp_suite}
			COMMAND uml_arm64_simd_facmp_${simd_facmp_suite}_tests)
	endforeach()

	foreach(simd_fcvt_vector_suite IN ITEMS allocation exact rounding control)
		add_executable(
			uml_arm64_simd_fcvt_vector_${simd_fcvt_vector_suite}_tests
			tests/arm64_simd_fcvt_vector_${simd_fcvt_vector_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fcvt_vector_${simd_fcvt_vector_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_fcvt_vector_${simd_fcvt_vector_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-fcvt-vector-${simd_fcvt_vector_suite}
			COMMAND
			uml_arm64_simd_fcvt_vector_${simd_fcvt_vector_suite}_tests)
	endforeach()

	foreach(simd_fcvtxn_suite IN ITEMS allocation exact rounding control)
		add_executable(uml_arm64_simd_fcvtxn_${simd_fcvtxn_suite}_tests
			tests/arm64_simd_fcvtxn_${simd_fcvtxn_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fcvtxn_${simd_fcvtxn_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_simd_fcvtxn_${simd_fcvtxn_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		add_test(NAME arm64-simd-fcvtxn-${simd_fcvtxn_suite}
			COMMAND uml_arm64_simd_fcvtxn_${simd_fcvtxn_suite}_tests)
	endforeach()

	foreach(simd_fcvtzi_suite IN ITEMS gpr_allocation gpr_behavior gpr_control
		simd_allocation simd_behavior simd_control fixed_gpr_allocation
		fixed_gpr_behavior fixed_simd_allocation fixed_simd_behavior
		fixed_control)
		add_executable(uml_arm64_simd_fcvtzi_${simd_fcvtzi_suite}_tests
			tests/arm64_simd_fcvtzi_${simd_fcvtzi_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fcvtzi_${simd_fcvtzi_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_simd_fcvtzi_${simd_fcvtzi_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		add_test(NAME arm64-simd-fcvtzi-${simd_fcvtzi_suite}
			COMMAND uml_arm64_simd_fcvtzi_${simd_fcvtzi_suite}_tests)
	endforeach()

	foreach(simd_fcvtri_suite IN ITEMS gpr_rounding simd_rounding
		fpr_allocation fpr_behavior control reserved)
		add_executable(uml_arm64_simd_fcvtri_${simd_fcvtri_suite}_tests
			tests/arm64_simd_fcvtri_${simd_fcvtri_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fcvtri_${simd_fcvtri_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_simd_fcvtri_${simd_fcvtri_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		add_test(NAME arm64-simd-fcvtri-${simd_fcvtri_suite}
			COMMAND uml_arm64_simd_fcvtri_${simd_fcvtri_suite}_tests)
	endforeach()

	foreach(simd_cvtfi_suite IN ITEMS gpr simd fpr rounding control
		fixed_gpr fixed_simd fixed_behavior)
		add_executable(uml_arm64_simd_cvtfi_${simd_cvtfi_suite}_tests
			tests/arm64_simd_cvtfi_${simd_cvtfi_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_cvtfi_${simd_cvtfi_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_simd_cvtfi_${simd_cvtfi_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		add_test(NAME arm64-simd-cvtfi-${simd_cvtfi_suite}
			COMMAND uml_arm64_simd_cvtfi_${simd_cvtfi_suite}_tests)
	endforeach()

	foreach(simd_fjcvt_suite IN ITEMS allocation behavior control state)
		add_executable(uml_arm64_simd_fjcvt_${simd_fjcvt_suite}_tests
			tests/arm64_simd_fjcvt_${simd_fjcvt_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_fjcvt_${simd_fjcvt_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_simd_fjcvt_${simd_fjcvt_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		add_test(NAME arm64-simd-fjcvt-${simd_fjcvt_suite}
			COMMAND uml_arm64_simd_fjcvt_${simd_fjcvt_suite}_tests)
	endforeach()

	foreach(simd_sha_suite IN ITEMS sha12_allocation sha1_behavior sha256_behavior
		sha512_allocation sha512_behavior sm_allocation sm3_behavior sm4_behavior)
		add_executable(uml_arm64_simd_${simd_sha_suite}_tests
			tests/arm64_simd_${simd_sha_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_${simd_sha_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_${simd_sha_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-${simd_sha_suite}
			COMMAND uml_arm64_simd_${simd_sha_suite}_tests)
	endforeach()

	foreach(fp8_suite IN ITEMS allocation convert_up convert_down control dot
		multiply matrix fpmr gadget)
		add_executable(uml_arm64_fp8_${fp8_suite}_tests
			tests/arm64_fp8_${fp8_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(uml_arm64_fp8_${fp8_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_fp8_${fp8_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_compile_definitions(uml_arm64_fp8_${fp8_suite}_tests PRIVATE
			UML_TEST_GADGET_INTERPRETER_RUNPATH=$<BOOL:${UML_ENABLE_GADGET_INTERPRETER_RUNPATH}>)
		add_test(NAME arm64-fp8-${fp8_suite}
			COMMAND uml_arm64_fp8_${fp8_suite}_tests)
	endforeach()
endif()

install(TARGETS uml_backend
    ARCHIVE DESTINATION lib
    PUBLIC_HEADER DESTINATION include/uml)
