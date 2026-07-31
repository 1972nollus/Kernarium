# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_arm64_dispatch OBJECT
    cpu/arm64/dispatcher.cpp
    cpu/arm64/instructions/control_flow.cpp
    cpu/arm64/instructions/crc32.cpp
    cpu/arm64/instructions/floating_point.cpp
    cpu/arm64/instructions/gcs.cpp
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
target_include_directories(uml_arm64_gadgets PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_arm64_gadgets PUBLIC cxx_std_20)
target_compile_options(uml_arm64_gadgets PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>)
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

set(UML_STATIC_RUNTIME_SOURCES
    runtime/jit/jit_compiler_crypto.cpp
    runtime/jit/jit_compiler_floating.cpp
    runtime/jit/jit_compiler_integer.cpp
    runtime/jit/jit_compiler_simd_arithmetic.cpp
    runtime/jit/jit_executor.cpp
    runtime/jit/jit_tlb_translate.S
    runtime/static_guest.cpp
    runtime/static/gadget_executor.cpp
    runtime/static/guest_memory.cpp)

add_library(uml_static_runtime OBJECT
    ${UML_STATIC_RUNTIME_SOURCES})
target_include_directories(uml_static_runtime PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_static_runtime PUBLIC cxx_std_20)
target_compile_options(uml_static_runtime PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(uml_static_runtime PRIVATE
    UML_JIT_PUBLICATION_GATE_TESTING=0)
set_target_properties(uml_static_runtime PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

if(NOT UML_GUEST_16K_PAGES)
    add_library(uml_native_mmu OBJECT
        ${PROJECT_SOURCE_DIR}/darwin/runtime/mmu/native_address_space.cpp)
    target_include_directories(uml_native_mmu PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/runtime/mmu)
    target_compile_features(uml_native_mmu PUBLIC cxx_std_20)
    target_compile_options(uml_native_mmu PRIVATE -Wall -Wextra -Werror)
    set_target_properties(uml_native_mmu PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    set(UML_NATIVE_MMU_OBJECTS $<TARGET_OBJECTS:uml_native_mmu>)
endif()

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
    ${UML_NATIVE_MMU_OBJECTS})

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
    UML_DEFAULT_EXECUTION_ENGINE_JIT=$<BOOL:${UML_TARGET_MACOS}>
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

if(UML_BUILD_TESTS AND NOT UML_GUEST_16K_PAGES)
    add_executable(uml_native_mmu_tests
        tests/native_mmu_tests.cpp
        $<TARGET_OBJECTS:uml_native_mmu>)
    target_include_directories(uml_native_mmu_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/runtime/mmu)
    target_compile_features(uml_native_mmu_tests PRIVATE cxx_std_20)
    target_compile_options(uml_native_mmu_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME native-4k-shadow-mmu COMMAND uml_native_mmu_tests)
endif()

if(UML_BUILD_TESTS)
    add_library(uml_static_runtime_publication_gate_testing OBJECT
        ${UML_STATIC_RUNTIME_SOURCES})
    target_include_directories(
        uml_static_runtime_publication_gate_testing PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(
        uml_static_runtime_publication_gate_testing PUBLIC cxx_std_20)
    target_compile_options(
        uml_static_runtime_publication_gate_testing PRIVATE
        -Wall -Wextra -Werror)
    target_compile_definitions(
        uml_static_runtime_publication_gate_testing PRIVATE
        UML_JIT_PUBLICATION_GATE_TESTING=1)
    set_target_properties(
        uml_static_runtime_publication_gate_testing PROPERTIES
        POSITION_INDEPENDENT_CODE ON)

    add_library(uml_arm64_engine_test_support STATIC
        tests/arm64_engine_test_support.cpp)
    target_link_libraries(uml_arm64_engine_test_support PRIVATE UML::Backend)
    target_include_directories(uml_arm64_engine_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_features(uml_arm64_engine_test_support PUBLIC cxx_std_20)
    target_compile_options(uml_arm64_engine_test_support PRIVATE
        -Wall -Wextra -Werror)

    add_executable(uml_arm64_engine_test_support_tests
        tests/arm64_engine_test_support_tests.cpp)
    target_link_libraries(uml_arm64_engine_test_support_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(uml_arm64_engine_test_support_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-engine-harness
        COMMAND uml_arm64_engine_test_support_tests)

    add_executable(uml_arm64_deferred_fp_state_tests
        tests/arm64_deferred_fp_state_tests.cpp)
    target_link_libraries(uml_arm64_deferred_fp_state_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(uml_arm64_deferred_fp_state_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-deferred-fp-state
        COMMAND uml_arm64_deferred_fp_state_tests)

    add_executable(uml_host_instruction_probe_tests
        tests/host_instruction_probe_tests.cpp)
    target_include_directories(uml_host_instruction_probe_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(uml_host_instruction_probe_tests PRIVATE cxx_std_20)
    target_compile_options(uml_host_instruction_probe_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-host-instruction-probe
        COMMAND uml_host_instruction_probe_tests)
    add_test(NAME arm64-instruction-inventory
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_a64_instruction_inventory.py)

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
    target_include_directories(uml_backend_api_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_features(uml_backend_api_tests PRIVATE cxx_std_20)
    target_compile_definitions(uml_backend_api_tests PRIVATE
        UML_BACKEND_TEST_HAS_SLIRP=$<BOOL:${UML_BUILD_KERNEL_RUNNER}>)
    target_compile_options(uml_backend_api_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME backend-native-entry-api COMMAND uml_backend_api_tests)

    add_executable(uml_arm64_dispatcher_internal_tests
        tests/arm64_dispatcher_internal_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>)
    target_include_directories(uml_arm64_dispatcher_internal_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(uml_arm64_dispatcher_internal_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-dispatcher-internal
        COMMAND uml_arm64_dispatcher_internal_tests)

    add_executable(uml_arm64_floating_exception_tests
        tests/arm64_floating_exception_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>)
    target_include_directories(uml_arm64_floating_exception_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(uml_arm64_floating_exception_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-floating-exception
        COMMAND uml_arm64_floating_exception_tests)

    foreach(floating_trap_suite IN ITEMS scalar vector control)
        add_executable(uml_arm64_floating_trap_${floating_trap_suite}_tests
            tests/arm64_floating_trap_${floating_trap_suite}_tests.cpp
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_static_mmu>
            $<TARGET_OBJECTS:uml_arm64_gadgets>)
        target_include_directories(
            uml_arm64_floating_trap_${floating_trap_suite}_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_floating_trap_${floating_trap_suite}_tests PRIVATE
            -Wall -Wextra -Werror)
        target_link_libraries(
            uml_arm64_floating_trap_${floating_trap_suite}_tests PRIVATE
            uml_arm64_engine_test_support)
        add_test(NAME arm64-floating-trap-${floating_trap_suite}
            COMMAND
                uml_arm64_floating_trap_${floating_trap_suite}_tests)
    endforeach()

    add_executable(uml_arm64_floating_trap_runtime_tests
        tests/arm64_floating_trap_runtime_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(uml_arm64_floating_trap_runtime_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_floating_trap_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-floating-trap-runtime
        COMMAND uml_arm64_floating_trap_runtime_tests)

    add_executable(uml_arm64_gadget_terminal_count_tests
        tests/arm64_gadget_terminal_count_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(uml_arm64_gadget_terminal_count_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_terminal_count_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-gadget-terminal-count
        COMMAND uml_arm64_gadget_terminal_count_tests)

    add_executable(uml_arm64_static_runtime_tests
        tests/arm64_static_runtime_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(uml_arm64_static_runtime_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_static_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-static-runtime COMMAND uml_arm64_static_runtime_tests)

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

    foreach(JIT_TEST IN ITEMS
            runtime
            capability
            cache_validation
            barrier_codegen
            barrier_runtime
            illegal_runtime
            publication_gate
            reservation_alias)
        if(JIT_TEST STREQUAL "publication_gate")
            set(UML_JIT_RUNTIME_OBJECTS
                $<TARGET_OBJECTS:uml_static_runtime_publication_gate_testing>)
        else()
            set(UML_JIT_RUNTIME_OBJECTS
                $<TARGET_OBJECTS:uml_static_runtime>)
        endif()
        add_executable(uml_arm64_jit_${JIT_TEST}_tests
            tests/arm64_jit_${JIT_TEST}_tests.cpp
            runtime/guest_selector.cpp
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_static_mmu>
            ${UML_JIT_RUNTIME_OBJECTS})
        target_include_directories(uml_arm64_jit_${JIT_TEST}_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(uml_arm64_jit_${JIT_TEST}_tests PRIVATE
            -Wall -Wextra -Werror)
        add_test(NAME arm64-jit-${JIT_TEST}
            COMMAND uml_arm64_jit_${JIT_TEST}_tests)
    endforeach()

    add_executable(uml_arm64_jit_lazy_flags_runtime_tests
        tests/arm64_jit_lazy_flags_runtime_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(
        uml_arm64_jit_lazy_flags_runtime_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_lazy_flags_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-lazy-flags-runtime
        COMMAND uml_arm64_jit_lazy_flags_runtime_tests)

    add_executable(uml_arm64_jit_lazy_flags_codegen_tests
        tests/arm64_jit_lazy_flags_codegen_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(
        uml_arm64_jit_lazy_flags_codegen_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_lazy_flags_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-lazy-flags-codegen
        COMMAND uml_arm64_jit_lazy_flags_codegen_tests)

    foreach(JIT_INDIRECT_LINK_TEST IN ITEMS codegen runtime)
        add_executable(
            uml_arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests
            tests/arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests.cpp
            runtime/guest_selector.cpp
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_static_mmu>
            $<TARGET_OBJECTS:uml_static_runtime>)
        target_include_directories(
            uml_arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests
            PRIVATE -Wall -Wextra -Werror)
        add_test(
            NAME arm64-jit-indirect-link-${JIT_INDIRECT_LINK_TEST}
            COMMAND
                uml_arm64_jit_indirect_link_${JIT_INDIRECT_LINK_TEST}_tests)
    endforeach()

    foreach(JIT_CHAIN_ABI_TEST IN ITEMS codegen runtime)
        add_executable(
            uml_arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests
            tests/arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests.cpp
            runtime/guest_selector.cpp
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_static_mmu>
            $<TARGET_OBJECTS:uml_static_runtime>)
        if(JIT_CHAIN_ABI_TEST STREQUAL "runtime")
            target_sources(
                uml_arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests PRIVATE
                tests/arm64_jit_chain_abi_probe.S)
        endif()
        target_include_directories(
            uml_arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests PRIVATE
            -Wall -Wextra -Werror)
        add_test(
            NAME arm64-jit-chain-abi-${JIT_CHAIN_ABI_TEST}
            COMMAND uml_arm64_jit_chain_abi_${JIT_CHAIN_ABI_TEST}_tests)
    endforeach()

    foreach(JIT_EXACT_RETIREMENT_TEST IN ITEMS codegen runtime)
        add_executable(
            uml_arm64_jit_exact_retirement_${JIT_EXACT_RETIREMENT_TEST}_tests
            tests/arm64_jit_exact_retirement_${JIT_EXACT_RETIREMENT_TEST}_tests.cpp
            runtime/guest_selector.cpp
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_static_mmu>
            $<TARGET_OBJECTS:uml_static_runtime>)
        target_include_directories(
            uml_arm64_jit_exact_retirement_${JIT_EXACT_RETIREMENT_TEST}_tests
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_jit_exact_retirement_${JIT_EXACT_RETIREMENT_TEST}_tests
            PRIVATE -Wall -Wextra -Werror)
        add_test(
            NAME arm64-jit-exact-retirement-${JIT_EXACT_RETIREMENT_TEST}
            COMMAND
                uml_arm64_jit_exact_retirement_${JIT_EXACT_RETIREMENT_TEST}_tests)
    endforeach()

    add_executable(uml_arm64_jit_fp_link_runtime_tests
        tests/arm64_jit_fp_link_runtime_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(
        uml_arm64_jit_fp_link_runtime_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_fp_link_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-fp-link-runtime
        COMMAND uml_arm64_jit_fp_link_runtime_tests)

    add_executable(uml_arm64_counter_codegen_tests
        tests/arm64_counter_codegen_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(uml_arm64_counter_codegen_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_counter_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-counter-codegen
        COMMAND uml_arm64_counter_codegen_tests)

    add_executable(uml_arm64_jit_counter_runtime_tests
        tests/arm64_jit_counter_runtime_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(uml_arm64_jit_counter_runtime_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_jit_counter_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-counter-runtime
        COMMAND uml_arm64_jit_counter_runtime_tests)

    add_executable(uml_arm64_jit_register_remap_codegen_tests
        tests/arm64_jit_register_remap_codegen_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(
        uml_arm64_jit_register_remap_codegen_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_register_remap_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-register-remap-codegen
        COMMAND uml_arm64_jit_register_remap_codegen_tests)

    foreach(JIT_ONE_TO_ONE_TEST IN ITEMS base fp_simd)
        add_executable(
            uml_arm64_jit_one_to_one_${JIT_ONE_TO_ONE_TEST}_codegen_tests
            tests/arm64_jit_one_to_one_${JIT_ONE_TO_ONE_TEST}_codegen_tests.cpp
            runtime/guest_selector.cpp
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_static_mmu>
            $<TARGET_OBJECTS:uml_static_runtime>)
        target_include_directories(
            uml_arm64_jit_one_to_one_${JIT_ONE_TO_ONE_TEST}_codegen_tests
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_jit_one_to_one_${JIT_ONE_TO_ONE_TEST}_codegen_tests
            PRIVATE
            -Wall -Wextra -Werror)
        add_test(
            NAME arm64-jit-one-to-one-${JIT_ONE_TO_ONE_TEST}-codegen
            COMMAND
                uml_arm64_jit_one_to_one_${JIT_ONE_TO_ONE_TEST}_codegen_tests)
    endforeach()

    add_executable(uml_arm64_jit_register_remap_behavior_tests
        tests/arm64_jit_register_remap_behavior_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_register_remap_behavior_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_register_remap_behavior_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-register-remap-behavior
        COMMAND uml_arm64_jit_register_remap_behavior_tests)

    add_executable(uml_arm64_jit_tlb_miss_preservation_tests
        tests/arm64_jit_tlb_miss_preservation_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_tlb_miss_preservation_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_tlb_miss_preservation_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-tlb-miss-preservation
        COMMAND uml_arm64_jit_tlb_miss_preservation_tests)

    add_executable(uml_arm64_jit_sp_memory_run_codegen_tests
        tests/arm64_jit_sp_memory_run_codegen_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(
        uml_arm64_jit_sp_memory_run_codegen_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_sp_memory_run_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-sp-memory-run-codegen
        COMMAND uml_arm64_jit_sp_memory_run_codegen_tests)

    add_executable(uml_arm64_jit_sp_memory_run_runtime_tests
        tests/arm64_jit_sp_memory_run_runtime_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_sp_memory_run_runtime_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_sp_memory_run_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-sp-memory-run-runtime
        COMMAND uml_arm64_jit_sp_memory_run_runtime_tests)

    add_executable(uml_arm64_jit_gpr_memory_run_codegen_tests
        tests/arm64_jit_gpr_memory_run_codegen_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(
        uml_arm64_jit_gpr_memory_run_codegen_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_gpr_memory_run_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-gpr-memory-run-codegen
        COMMAND uml_arm64_jit_gpr_memory_run_codegen_tests)

    add_executable(uml_arm64_jit_gpr_memory_run_runtime_tests
        tests/arm64_jit_gpr_memory_run_runtime_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_gpr_memory_run_runtime_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_gpr_memory_run_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-gpr-memory-run-runtime
        COMMAND uml_arm64_jit_gpr_memory_run_runtime_tests)

    add_executable(uml_arm64_jit_simd_memory_run_codegen_tests
        tests/arm64_jit_simd_memory_run_codegen_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(
        uml_arm64_jit_simd_memory_run_codegen_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_simd_memory_run_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-simd-memory-run-codegen
        COMMAND uml_arm64_jit_simd_memory_run_codegen_tests)

    add_executable(uml_arm64_jit_simd_memory_run_runtime_tests
        tests/arm64_jit_simd_memory_run_runtime_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_simd_memory_run_runtime_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_simd_memory_run_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-simd-memory-run-runtime
        COMMAND uml_arm64_jit_simd_memory_run_runtime_tests)

    add_executable(uml_arm64_jit_scalar_atomic_codegen_tests
        tests/arm64_jit_scalar_atomic_codegen_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(
        uml_arm64_jit_scalar_atomic_codegen_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_scalar_atomic_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-scalar-atomic-codegen
        COMMAND uml_arm64_jit_scalar_atomic_codegen_tests)

    add_executable(uml_arm64_jit_simd_structure_remap_codegen_tests
        tests/arm64_jit_simd_structure_remap_codegen_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(
        uml_arm64_jit_simd_structure_remap_codegen_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_simd_structure_remap_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-simd-structure-remap-codegen
        COMMAND uml_arm64_jit_simd_structure_remap_codegen_tests)

    add_executable(uml_arm64_jit_simd_structure_remap_runtime_tests
        tests/arm64_jit_simd_structure_remap_runtime_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_simd_structure_remap_runtime_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_simd_structure_remap_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-simd-structure-remap-runtime
        COMMAND uml_arm64_jit_simd_structure_remap_runtime_tests)

    add_executable(uml_arm64_jit_vector_remap_codegen_tests
        tests/arm64_jit_vector_remap_codegen_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(
        uml_arm64_jit_vector_remap_codegen_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_vector_remap_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-vector-remap-codegen
        COMMAND uml_arm64_jit_vector_remap_codegen_tests)

    add_executable(uml_arm64_jit_table_list_remap_codegen_tests
        tests/arm64_jit_table_list_remap_codegen_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(
        uml_arm64_jit_table_list_remap_codegen_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_table_list_remap_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-table-list-remap-codegen
        COMMAND uml_arm64_jit_table_list_remap_codegen_tests)

    add_executable(uml_arm64_jit_table_list_remap_runtime_tests
        tests/arm64_jit_table_list_remap_runtime_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_table_list_remap_runtime_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_table_list_remap_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-table-list-remap-runtime
        COMMAND uml_arm64_jit_table_list_remap_runtime_tests)

    add_executable(uml_arm64_jit_gpr_residency_codegen_tests
        tests/arm64_jit_gpr_residency_codegen_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(
        uml_arm64_jit_gpr_residency_codegen_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_gpr_residency_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-gpr-residency-codegen
        COMMAND uml_arm64_jit_gpr_residency_codegen_tests)

    add_executable(uml_arm64_jit_gpr_residency_runtime_tests
        tests/arm64_jit_gpr_residency_runtime_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_gpr_residency_runtime_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_gpr_residency_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-gpr-residency-runtime
        COMMAND uml_arm64_jit_gpr_residency_runtime_tests)

    add_executable(uml_arm64_jit_pair_remap_codegen_tests
        tests/arm64_jit_pair_remap_codegen_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(
        uml_arm64_jit_pair_remap_codegen_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_pair_remap_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-pair-remap-codegen
        COMMAND uml_arm64_jit_pair_remap_codegen_tests)

    add_executable(uml_arm64_jit_control_remap_codegen_tests
        tests/arm64_jit_control_remap_codegen_tests.cpp
        runtime/guest_selector.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(
        uml_arm64_jit_control_remap_codegen_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_jit_control_remap_codegen_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-control-remap-codegen
        COMMAND uml_arm64_jit_control_remap_codegen_tests)

    add_executable(uml_arm64_jit_control_remap_behavior_tests
        tests/arm64_jit_control_remap_behavior_tests.cpp)
    target_link_libraries(
        uml_arm64_jit_control_remap_behavior_tests PRIVATE
        uml_arm64_engine_test_support)
    target_compile_options(
        uml_arm64_jit_control_remap_behavior_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-jit-control-remap-behavior
        COMMAND uml_arm64_jit_control_remap_behavior_tests)

    if(DARWIN_UML_JIT_GPR_LINK_PROFILE)
        add_executable(uml_arm64_jit_gpr_link_profile_codegen_tests
            tests/arm64_jit_gpr_link_profile_codegen_tests.cpp
            runtime/guest_selector.cpp
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_static_mmu>
            $<TARGET_OBJECTS:uml_static_runtime>)
        target_include_directories(
            uml_arm64_jit_gpr_link_profile_codegen_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_jit_gpr_link_profile_codegen_tests PRIVATE
            -Wall -Wextra -Werror)
        add_test(NAME arm64-jit-gpr-link-profile-codegen
            COMMAND uml_arm64_jit_gpr_link_profile_codegen_tests)

        add_executable(uml_arm64_jit_gpr_link_profile_runtime_tests
            tests/arm64_jit_gpr_link_profile_runtime_tests.cpp
            runtime/guest_selector.cpp
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_static_mmu>
            $<TARGET_OBJECTS:uml_static_runtime>)
        target_include_directories(
            uml_arm64_jit_gpr_link_profile_runtime_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_jit_gpr_link_profile_runtime_tests PRIVATE
            -Wall -Wextra -Werror)
        add_test(NAME arm64-jit-gpr-link-profile-runtime
            COMMAND uml_arm64_jit_gpr_link_profile_runtime_tests)
    endif()

    if(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
        add_executable(uml_arm64_jit_gpr_pressure_profile_tests
            tests/arm64_jit_gpr_pressure_profile_tests.cpp
            runtime/guest_selector.cpp
            $<TARGET_OBJECTS:uml_arm64_dispatch>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_static_mmu>
            $<TARGET_OBJECTS:uml_static_runtime>)
        target_include_directories(
            uml_arm64_jit_gpr_pressure_profile_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
            uml_arm64_jit_gpr_pressure_profile_tests PRIVATE
            -Wall -Wextra -Werror)
        add_test(NAME arm64-jit-gpr-pressure-profile
            COMMAND uml_arm64_jit_gpr_pressure_profile_tests)
    endif()

    add_executable(uml_arm64_gadget_runtime_tests
        tests/arm64_gadget_runtime_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_gadget_runtime_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_runtime_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-gadget-runtime
        COMMAND uml_arm64_gadget_runtime_tests)

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

    foreach(base_arithmetic_suite IN ITEMS allocation carry divide variable_shift)
        add_executable(
			uml_arm64_base_arithmetic_${base_arithmetic_suite}_tests
			tests/arm64_base_arithmetic_${base_arithmetic_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
        target_include_directories(
			uml_arm64_base_arithmetic_${base_arithmetic_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_arithmetic_${base_arithmetic_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_arithmetic_${base_arithmetic_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        string(REPLACE "_" "-" base_arithmetic_test_name
			"${base_arithmetic_suite}")
        add_test(NAME arm64-base-arithmetic-${base_arithmetic_test_name}
			COMMAND
			uml_arm64_base_arithmetic_${base_arithmetic_suite}_tests)
    endforeach()

    foreach(base_bit_suite IN ITEMS allocation behavior reserved)
        add_executable(uml_arm64_base_bit_${base_bit_suite}_tests
			tests/arm64_base_bit_${base_bit_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
        target_include_directories(
			uml_arm64_base_bit_${base_bit_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_bit_${base_bit_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_bit_${base_bit_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        add_test(NAME arm64-base-bit-${base_bit_suite}
			COMMAND uml_arm64_base_bit_${base_bit_suite}_tests)
    endforeach()

    add_executable(uml_arm64_base_multiply_tests
		tests/arm64_base_multiply_tests.cpp
		tests/arm64_base_multiply_allocation_tests.cpp
		tests/arm64_base_multiply_behavior_tests.cpp
		tests/arm64_base_multiply_reserved_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_multiply_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_multiply_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_multiply_tests PRIVATE
        uml_arm64_engine_test_support)
    foreach(base_multiply_suite IN ITEMS allocation behavior reserved)
        add_test(NAME arm64-base-multiply-${base_multiply_suite}
			COMMAND uml_arm64_base_multiply_tests
				${base_multiply_suite})
    endforeach()

    foreach(base_condition_suite IN ITEMS
		allocation
		select
		compare_immediate
		compare_register
		structure)
        add_executable(uml_arm64_base_condition_${base_condition_suite}_tests
			tests/arm64_base_condition_${base_condition_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
        target_include_directories(
			uml_arm64_base_condition_${base_condition_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_condition_${base_condition_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_condition_${base_condition_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        string(REPLACE "_" "-" base_condition_test_name
			"${base_condition_suite}")
        add_test(NAME arm64-base-condition-${base_condition_test_name}
			COMMAND
			uml_arm64_base_condition_${base_condition_suite}_tests)
    endforeach()

    foreach(base_min_max_suite IN ITEMS allocation immediate register structure)
        add_executable(uml_arm64_base_min_max_${base_min_max_suite}_tests
			tests/arm64_base_min_max_${base_min_max_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
        target_include_directories(
			uml_arm64_base_min_max_${base_min_max_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_min_max_${base_min_max_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_min_max_${base_min_max_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        add_test(NAME arm64-base-min-max-${base_min_max_suite}
			COMMAND uml_arm64_base_min_max_${base_min_max_suite}_tests)
    endforeach()

    add_executable(uml_arm64_base_add_sub_tests
		tests/arm64_base_add_sub_tests.cpp
		tests/arm64_base_add_sub_immediate_tests.cpp
		tests/arm64_base_add_sub_shifted_tests.cpp
		tests/arm64_base_add_sub_extended_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_add_sub_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_add_sub_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_add_sub_tests PRIVATE
        uml_arm64_engine_test_support)
    foreach(base_add_sub_suite IN ITEMS immediate shifted extended)
        add_test(NAME arm64-base-add-sub-${base_add_sub_suite}
            COMMAND uml_arm64_base_add_sub_tests ${base_add_sub_suite})
    endforeach()

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
    target_link_libraries(uml_arm64_base_mte_pointer_tests PRIVATE
		uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_mte_tag_tests PRIVATE
		uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_mte_random_tag_tests PRIVATE
		uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_cpa_pointer_tests PRIVATE
		uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_cpa_multiply_tests PRIVATE
		uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_cmpbr_immediate_tests PRIVATE
		uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_cmpbr_register_tests PRIVATE
		uml_arm64_engine_test_support)
    add_test(NAME arm64-base-cmpbr-register
		COMMAND uml_arm64_base_cmpbr_register_tests)

    add_executable(uml_arm64_base_logical_tests
		tests/arm64_base_logical_tests.cpp
		tests/arm64_base_logical_immediate_tests.cpp
		tests/arm64_base_logical_shifted_tests.cpp
		tests/arm64_base_logical_behavior_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_logical_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_logical_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_logical_tests PRIVATE
        uml_arm64_engine_test_support)
    add_test(NAME arm64-base-logical-immediate
		COMMAND uml_arm64_base_logical_tests immediate)
    add_test(NAME arm64-base-logical-shifted
		COMMAND uml_arm64_base_logical_tests shifted)
    add_test(NAME arm64-base-logical-behavior
		COMMAND uml_arm64_base_logical_tests behavior)

    foreach(base_move_wide_suite IN ITEMS allocation behavior reserved)
        add_executable(uml_arm64_base_move_wide_${base_move_wide_suite}_tests
			tests/arm64_base_move_wide_${base_move_wide_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
        target_include_directories(
			uml_arm64_base_move_wide_${base_move_wide_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_move_wide_${base_move_wide_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_move_wide_${base_move_wide_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        add_test(NAME arm64-base-move-wide-${base_move_wide_suite}
			COMMAND
			uml_arm64_base_move_wide_${base_move_wide_suite}_tests)
    endforeach()

    foreach(base_integer_alias IN ITEMS mov_inverted rev64)
        add_executable(uml_arm64_base_${base_integer_alias}_alias_tests
			tests/arm64_base_${base_integer_alias}_alias_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
        target_include_directories(
			uml_arm64_base_${base_integer_alias}_alias_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_${base_integer_alias}_alias_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_${base_integer_alias}_alias_tests PRIVATE
			uml_arm64_engine_test_support)
        string(REPLACE "_" "-" base_integer_alias_test_name
			"${base_integer_alias}")
        add_test(NAME
			arm64-base-${base_integer_alias_test_name}-alias
			COMMAND
			uml_arm64_base_${base_integer_alias}_alias_tests)
    endforeach()

    foreach(base_bitfield_suite IN ITEMS allocation behavior reserved)
        add_executable(uml_arm64_base_bitfield_${base_bitfield_suite}_tests
			tests/arm64_base_bitfield_${base_bitfield_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
        target_include_directories(
			uml_arm64_base_bitfield_${base_bitfield_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_bitfield_${base_bitfield_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_bitfield_${base_bitfield_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        add_test(NAME arm64-base-bitfield-${base_bitfield_suite}
			COMMAND uml_arm64_base_bitfield_${base_bitfield_suite}_tests)
    endforeach()

    foreach(base_extract_suite IN ITEMS allocation behavior reserved)
        add_executable(uml_arm64_base_extract_${base_extract_suite}_tests
			tests/arm64_base_extract_${base_extract_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
        target_include_directories(
			uml_arm64_base_extract_${base_extract_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_extract_${base_extract_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_extract_${base_extract_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        add_test(NAME arm64-base-extract-${base_extract_suite}
			COMMAND uml_arm64_base_extract_${base_extract_suite}_tests)
    endforeach()

    foreach(base_control_suite IN ITEMS
		pc_relative
		direct_branch
		conditional_branch
		compare_test
		register_branch)
        add_executable(uml_arm64_base_control_${base_control_suite}_tests
			tests/arm64_base_control_${base_control_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
        target_include_directories(
			uml_arm64_base_control_${base_control_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_control_${base_control_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_control_${base_control_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        string(REPLACE "_" "-" base_control_test_name
			"${base_control_suite}")
        add_test(NAME arm64-base-control-${base_control_test_name}
			COMMAND
			uml_arm64_base_control_${base_control_suite}_tests)
    endforeach()

    foreach(base_crc_suite IN ITEMS
			allocation behavior reserved gadget fallback)
        add_executable(uml_arm64_base_crc_${base_crc_suite}_tests
			tests/arm64_base_crc_${base_crc_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
        target_include_directories(
			uml_arm64_base_crc_${base_crc_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_crc_${base_crc_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_crc_${base_crc_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        add_test(NAME arm64-base-crc-${base_crc_suite}
			COMMAND uml_arm64_base_crc_${base_crc_suite}_tests)
    endforeach()

    foreach(base_flag_suite IN ITEMS
		allocation fixed rmif setf reserved structure fallback)
        add_executable(uml_arm64_base_flag_${base_flag_suite}_tests
			tests/arm64_base_flag_${base_flag_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
        target_include_directories(
			uml_arm64_base_flag_${base_flag_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_flag_${base_flag_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_flag_${base_flag_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        add_test(NAME arm64-base-flag-${base_flag_suite}
			COMMAND uml_arm64_base_flag_${base_flag_suite}_tests)
    endforeach()

    foreach(base_barrier_suite IN ITEMS
		allocation behavior alias reserved structure)
        add_executable(uml_arm64_base_barrier_${base_barrier_suite}_tests
			tests/arm64_base_barrier_${base_barrier_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
        target_include_directories(
			uml_arm64_base_barrier_${base_barrier_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_barrier_${base_barrier_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_barrier_${base_barrier_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        add_test(NAME arm64-base-barrier-${base_barrier_suite}
			COMMAND uml_arm64_base_barrier_${base_barrier_suite}_tests)
    endforeach()

    foreach(base_hint_suite IN ITEMS
		allocation behavior feature structure)
        add_executable(uml_arm64_base_hint_${base_hint_suite}_tests
			tests/arm64_base_hint_${base_hint_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
        target_include_directories(
			uml_arm64_base_hint_${base_hint_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_options(
			uml_arm64_base_hint_${base_hint_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
        target_link_libraries(
			uml_arm64_base_hint_${base_hint_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
        add_test(NAME arm64-base-hint-${base_hint_suite}
			COMMAND uml_arm64_base_hint_${base_hint_suite}_tests)
    endforeach()

    add_executable(uml_arm64_base_hint_stateful_tests
		tests/arm64_base_hint_stateful_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_hint_stateful_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_hint_stateful_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_hint_stateful_tests PRIVATE
        uml_arm64_engine_test_support)
    add_test(NAME arm64-base-hint-stateful
        COMMAND uml_arm64_base_hint_stateful_tests)

    foreach(base_store_hint_suite IN ITEMS
		allocation behavior feature gadget)
	add_executable(
	    uml_arm64_base_store_hint_${base_store_hint_suite}_tests
	    tests/arm64_base_store_hint_${base_store_hint_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_arm64_dispatch>
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
	    uml_arm64_base_store_hint_${base_store_hint_suite}_tests PRIVATE
	    ${CMAKE_CURRENT_SOURCE_DIR}
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_base_store_hint_${base_store_hint_suite}_tests PRIVATE
	    -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_base_store_hint_${base_store_hint_suite}_tests PRIVATE
	    uml_arm64_engine_test_support)
	add_test(NAME arm64-base-store-hint-${base_store_hint_suite}
	    COMMAND
	    uml_arm64_base_store_hint_${base_store_hint_suite}_tests)
    endforeach()

    add_executable(uml_arm64_base_extended_hint_tests
		tests/arm64_base_extended_hint_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_extended_hint_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_extended_hint_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_extended_hint_tests PRIVATE
        uml_arm64_engine_test_support)
    foreach(EXTENDED_HINT_SHARD RANGE 0 7)
        add_test(NAME arm64-base-extended-hints-${EXTENDED_HINT_SHARD}
            COMMAND uml_arm64_base_extended_hint_tests
                ${EXTENDED_HINT_SHARD})
    endforeach()

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
    target_link_libraries(uml_arm64_base_system_register_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_cache_zero_tests PRIVATE
		uml_arm64_engine_test_support)
    add_test(NAME arm64-base-cache-zero
		COMMAND uml_arm64_base_cache_zero_tests)

    add_executable(uml_arm64_base_cache_zero_fault_tests
		tests/arm64_base_cache_zero_fault_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_cache_zero_fault_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_cache_zero_fault_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_cache_zero_fault_tests PRIVATE
		uml_arm64_engine_test_support)
    add_test(NAME arm64-base-cache-zero-faults
		COMMAND uml_arm64_base_cache_zero_fault_tests)

	foreach(SYSTEM_TEST IN ITEMS
			system_register_allocation
			system_instruction
			system_result_instruction
			system_alias)
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
		target_link_libraries(uml_arm64_base_${SYSTEM_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		if(SYSTEM_TEST STREQUAL "system_register_allocation")
			foreach(SYSTEM_REGISTER_SHARD RANGE 0 7)
				add_test(
					NAME arm64-base-system_register_allocation-${SYSTEM_REGISTER_SHARD}
					COMMAND uml_arm64_base_system_register_allocation_tests
						${SYSTEM_REGISTER_SHARD})
			endforeach()
		else()
			add_test(NAME arm64-base-${SYSTEM_TEST}
				COMMAND uml_arm64_base_${SYSTEM_TEST}_tests)
		endif()
	endforeach()

	foreach(SYSTEM_OPERATION_TEST IN ITEMS
			dc_operation
			dc_gadget
			tlbi_named
			tlbi_reserved
			tlbip_named
			tlbip_reserved
			sysp_allocation
			system_operation_gadget)
		add_executable(uml_arm64_base_${SYSTEM_OPERATION_TEST}_tests
			tests/arm64_base_${SYSTEM_OPERATION_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_base_${SYSTEM_OPERATION_TEST}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_base_${SYSTEM_OPERATION_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_base_${SYSTEM_OPERATION_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-base-${SYSTEM_OPERATION_TEST}
			COMMAND uml_arm64_base_${SYSTEM_OPERATION_TEST}_tests)
	endforeach()

	add_executable(uml_arm64_base_system_register_pair_tests
		tests/arm64_base_system_register_pair_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
		uml_arm64_base_system_register_pair_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_base_system_register_pair_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_base_system_register_pair_tests PRIVATE
		uml_arm64_engine_test_support)
	foreach(SYSTEM_REGISTER_PAIR_READ RANGE 0 1)
		foreach(SYSTEM_REGISTER_PAIR_OP1 RANGE 0 7)
			add_test(
				NAME arm64-base-system-register-pair-${SYSTEM_REGISTER_PAIR_READ}-${SYSTEM_REGISTER_PAIR_OP1}
				COMMAND uml_arm64_base_system_register_pair_tests
					${SYSTEM_REGISTER_PAIR_READ}
					${SYSTEM_REGISTER_PAIR_OP1})
		endforeach()
	endforeach()

	foreach(GCS_TEST IN ITEMS
			allocation
			behavior
			control
			memory
			fault
			reserved
			gadget)
		add_executable(uml_arm64_base_gcs_${GCS_TEST}_tests
			tests/arm64_base_gcs_${GCS_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(uml_arm64_base_gcs_${GCS_TEST}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_base_gcs_${GCS_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(uml_arm64_base_gcs_${GCS_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-base-gcs-${GCS_TEST}
			COMMAND uml_arm64_base_gcs_${GCS_TEST}_tests)
	endforeach()

	foreach(GIC_TEST IN ITEMS gic gicr gic_gadget)
		add_executable(uml_arm64_base_${GIC_TEST}_tests
			tests/arm64_base_${GIC_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(uml_arm64_base_${GIC_TEST}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_base_${GIC_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(uml_arm64_base_${GIC_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-base-${GIC_TEST}
			COMMAND uml_arm64_base_${GIC_TEST}_tests)
	endforeach()

	foreach(FEATURE_SYSTEM_TEST IN ITEMS
			prediction_restriction
			privileged_feature_alias
			feature_system_gadget)
		add_executable(uml_arm64_base_${FEATURE_SYSTEM_TEST}_tests
			tests/arm64_base_${FEATURE_SYSTEM_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_base_${FEATURE_SYSTEM_TEST}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_base_${FEATURE_SYSTEM_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_base_${FEATURE_SYSTEM_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-base-${FEATURE_SYSTEM_TEST}
			COMMAND uml_arm64_base_${FEATURE_SYSTEM_TEST}_tests)
	endforeach()

	foreach(CHKFEAT_TEST IN ITEMS chkfeat chkfeat_gadget)
		add_executable(uml_arm64_base_${CHKFEAT_TEST}_tests
			tests/arm64_base_${CHKFEAT_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_base_${CHKFEAT_TEST}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_base_${CHKFEAT_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_base_${CHKFEAT_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-base-${CHKFEAT_TEST}
			COMMAND uml_arm64_base_${CHKFEAT_TEST}_tests)
	endforeach()

	foreach(AT_TEST IN ITEMS at at_gadget)
		add_executable(uml_arm64_base_${AT_TEST}_tests
			tests/arm64_base_${AT_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(uml_arm64_base_${AT_TEST}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_base_${AT_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(uml_arm64_base_${AT_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-base-${AT_TEST}
			COMMAND uml_arm64_base_${AT_TEST}_tests)
	endforeach()

	foreach(IC_TEST IN ITEMS ic_privileged ic_ivau ic_gadget)
		add_executable(uml_arm64_base_${IC_TEST}_tests
			tests/arm64_base_${IC_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(uml_arm64_base_${IC_TEST}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(uml_arm64_base_${IC_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(uml_arm64_base_${IC_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-base-${IC_TEST}
			COMMAND uml_arm64_base_${IC_TEST}_tests)
	endforeach()

	foreach(PSTATE_TEST IN ITEMS allocation behavior access gadget)
		add_executable(uml_arm64_base_pstate_${PSTATE_TEST}_tests
			tests/arm64_base_pstate_${PSTATE_TEST}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_base_pstate_${PSTATE_TEST}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_base_pstate_${PSTATE_TEST}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_base_pstate_${PSTATE_TEST}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-base-pstate-${PSTATE_TEST}
			COMMAND uml_arm64_base_pstate_${PSTATE_TEST}_tests)
	endforeach()

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
    target_link_libraries(uml_arm64_base_pauth_strip_tests PRIVATE
        uml_arm64_engine_test_support)
    add_test(NAME arm64-base-pauth-strip
		COMMAND uml_arm64_base_pauth_strip_tests)

    add_executable(uml_arm64_base_pauth_profile_tests
		tests/arm64_base_pauth_profile_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_pauth_profile_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_pauth_profile_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_pauth_profile_tests PRIVATE
        uml_arm64_engine_test_support)
    add_test(NAME arm64-base-pauth-profile
		COMMAND uml_arm64_base_pauth_profile_tests)

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
    target_link_libraries(uml_arm64_base_pauth_code_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_pauth_auth_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_pauth_generic_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_pauth_branch_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_pauth_return_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_pauth_load_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_pauth_lr_data_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_pauth_lr_return_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_exception_tests PRIVATE
        uml_arm64_engine_test_support)
    add_test(NAME arm64-base-exceptions
        COMMAND uml_arm64_base_exception_tests)

    add_executable(uml_arm64_base_breakpoint_tests
		tests/arm64_base_breakpoint_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_base_breakpoint_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_base_breakpoint_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(uml_arm64_base_breakpoint_tests PRIVATE
        uml_arm64_engine_test_support)
    add_test(NAME arm64-base-breakpoints
        COMMAND uml_arm64_base_breakpoint_tests)

    add_executable(uml_arm64_base_undefined_exception_tests
		tests/arm64_base_undefined_exception_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(
        uml_arm64_base_undefined_exception_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_base_undefined_exception_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(
        uml_arm64_base_undefined_exception_tests PRIVATE
        uml_arm64_engine_test_support)
    add_test(NAME arm64-base-permanently-undefined
        COMMAND uml_arm64_base_undefined_exception_tests)
    add_test(NAME arm64-base-el0-undefined-exceptions
        COMMAND uml_arm64_base_undefined_exception_tests el0)

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
    target_link_libraries(uml_arm64_base_prefetch_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_range_prefetch_tests PRIVATE
        uml_arm64_engine_test_support)
    add_test(NAME arm64-base-range-prefetch
		COMMAND uml_arm64_base_range_prefetch_tests)

    foreach(base_ls64_suite IN ITEMS allocation behavior fault gadget)
	add_executable(uml_arm64_base_ls64_${base_ls64_suite}_tests
	    tests/arm64_base_ls64_${base_ls64_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_arm64_dispatch>
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
	    uml_arm64_base_ls64_${base_ls64_suite}_tests PRIVATE
	    ${CMAKE_CURRENT_SOURCE_DIR}
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_base_ls64_${base_ls64_suite}_tests PRIVATE
	    -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_base_ls64_${base_ls64_suite}_tests PRIVATE
	    uml_arm64_engine_test_support)
	string(REPLACE "_" "-" base_ls64_test_name "${base_ls64_suite}")
	add_test(NAME arm64-base-ls64-${base_ls64_test_name}
	    COMMAND uml_arm64_base_ls64_${base_ls64_suite}_tests)
    endforeach()

    foreach(base_mops_suite IN ITEMS
	    allocation copy_forward copy set set_tag set_tag_fault gadget)
	add_executable(uml_arm64_base_mops_${base_mops_suite}_tests
	    tests/arm64_base_mops_${base_mops_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_arm64_dispatch>
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
	    uml_arm64_base_mops_${base_mops_suite}_tests PRIVATE
	    ${CMAKE_CURRENT_SOURCE_DIR}
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_base_mops_${base_mops_suite}_tests PRIVATE
	    -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_base_mops_${base_mops_suite}_tests PRIVATE
	    uml_arm64_engine_test_support)
	string(REPLACE "_" "-" base_mops_test_name "${base_mops_suite}")
	add_test(NAME arm64-base-mops-${base_mops_test_name}
	    COMMAND uml_arm64_base_mops_${base_mops_suite}_tests)
    endforeach()

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
    target_link_libraries(uml_arm64_base_mte_tag_load_tests PRIVATE
		uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_mte_tag_store_tests PRIVATE
		uml_arm64_engine_test_support)
    add_test(NAME arm64-base-mte-tag-store
		COMMAND uml_arm64_base_mte_tag_store_tests)

    foreach(base_mte_data_suite IN ITEMS zero_store stgp)
	add_executable(uml_arm64_base_mte_${base_mte_data_suite}_tests
	    tests/arm64_base_mte_${base_mte_data_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_arm64_dispatch>
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
	    uml_arm64_base_mte_${base_mte_data_suite}_tests PRIVATE
	    ${CMAKE_CURRENT_SOURCE_DIR}
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_base_mte_${base_mte_data_suite}_tests PRIVATE
	    -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_base_mte_${base_mte_data_suite}_tests PRIVATE
	    uml_arm64_engine_test_support)
	add_test(NAME arm64-base-mte-${base_mte_data_suite}
	    COMMAND uml_arm64_base_mte_${base_mte_data_suite}_tests)
    endforeach()

    foreach(base_mte_multiple_suite IN ITEMS load store gadget)
	add_executable(
	    uml_arm64_base_mte_multiple_${base_mte_multiple_suite}_tests
	    tests/arm64_base_mte_multiple_${base_mte_multiple_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_arm64_dispatch>
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
	    uml_arm64_base_mte_multiple_${base_mte_multiple_suite}_tests PRIVATE
	    ${CMAKE_CURRENT_SOURCE_DIR}
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_base_mte_multiple_${base_mte_multiple_suite}_tests PRIVATE
	    -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_base_mte_multiple_${base_mte_multiple_suite}_tests PRIVATE
	    uml_arm64_engine_test_support)
	add_test(NAME arm64-base-mte-multiple-${base_mte_multiple_suite}
	    COMMAND
	    uml_arm64_base_mte_multiple_${base_mte_multiple_suite}_tests)
    endforeach()

    foreach(literal_load_suite allocation behavior fault)
	add_executable(uml_arm64_literal_load_${literal_load_suite}_tests
	    tests/arm64_literal_load_${literal_load_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_arm64_dispatch>
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
	    uml_arm64_literal_load_${literal_load_suite}_tests PRIVATE
	    ${CMAKE_CURRENT_SOURCE_DIR}
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_literal_load_${literal_load_suite}_tests PRIVATE
	    -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_literal_load_${literal_load_suite}_tests PRIVATE
	    uml_arm64_engine_test_support)
	add_test(NAME arm64-literal-load-${literal_load_suite}
	    COMMAND uml_arm64_literal_load_${literal_load_suite}_tests)
    endforeach()

    foreach(immediate_memory_suite allocation offset behavior fault structure)
	add_executable(uml_arm64_base_immediate_memory_${immediate_memory_suite}_tests
	    tests/arm64_base_immediate_memory_${immediate_memory_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_arm64_dispatch>
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
	    uml_arm64_base_immediate_memory_${immediate_memory_suite}_tests PRIVATE
	    ${CMAKE_CURRENT_SOURCE_DIR}
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_base_immediate_memory_${immediate_memory_suite}_tests PRIVATE
	    -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_base_immediate_memory_${immediate_memory_suite}_tests PRIVATE
	    uml_arm64_engine_test_support)
	add_test(NAME arm64-base-immediate-memory-${immediate_memory_suite}
	    COMMAND uml_arm64_base_immediate_memory_${immediate_memory_suite}_tests)
    endforeach()

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
    target_link_libraries(uml_arm64_base_load_store_register_tests PRIVATE
        uml_arm64_engine_test_support)
    add_test(NAME arm64-base-load-store-register
		COMMAND uml_arm64_base_load_store_register_tests)

    add_executable(uml_arm64_base_load_store_register_allocation_tests
		tests/arm64_base_load_store_register_allocation_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(
        uml_arm64_base_load_store_register_allocation_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(
        uml_arm64_base_load_store_register_allocation_tests PRIVATE
		-Wall -Wextra -Werror)
    target_link_libraries(
        uml_arm64_base_load_store_register_allocation_tests PRIVATE
        uml_arm64_engine_test_support)
    add_test(NAME arm64-base-load-store-register-allocation
		COMMAND uml_arm64_base_load_store_register_allocation_tests)

    foreach(load_store_register_suite fault prefetch structure)
	add_executable(
	    uml_arm64_base_load_store_register_${load_store_register_suite}_tests
	    tests/arm64_base_load_store_register_${load_store_register_suite}_tests.cpp
	    $<TARGET_OBJECTS:uml_arm64_dispatch>
	    $<TARGET_OBJECTS:uml_static_mmu>
	    $<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
	    uml_arm64_base_load_store_register_${load_store_register_suite}_tests
	    PRIVATE
	    ${CMAKE_CURRENT_SOURCE_DIR}
	    ${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
	    uml_arm64_base_load_store_register_${load_store_register_suite}_tests
	    PRIVATE -Wall -Wextra -Werror)
	target_link_libraries(
	    uml_arm64_base_load_store_register_${load_store_register_suite}_tests
	    PRIVATE uml_arm64_engine_test_support)
	add_test(NAME arm64-base-load-store-register-${load_store_register_suite}
	    COMMAND
	    uml_arm64_base_load_store_register_${load_store_register_suite}_tests)
    endforeach()

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
    target_link_libraries(uml_arm64_base_load_store_pair_tests PRIVATE
        uml_arm64_engine_test_support)
    add_test(NAME arm64-base-load-store-pair
		COMMAND uml_arm64_base_load_store_pair_tests)

    foreach(lsui_pair_suite IN ITEMS allocation behavior fault gadget)
	add_executable(uml_arm64_base_lsui_pair_${lsui_pair_suite}_tests
		tests/arm64_base_lsui_pair_${lsui_pair_suite}_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
		uml_arm64_base_lsui_pair_${lsui_pair_suite}_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_base_lsui_pair_${lsui_pair_suite}_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_base_lsui_pair_${lsui_pair_suite}_tests PRIVATE
		uml_arm64_engine_test_support)
	add_test(NAME arm64-base-lsui-pair-${lsui_pair_suite}
		COMMAND uml_arm64_base_lsui_pair_${lsui_pair_suite}_tests)
    endforeach()

    foreach(ordered_suite IN ITEMS allocation behavior fault gadget)
	add_executable(uml_arm64_base_ordered_${ordered_suite}_tests
		tests/arm64_base_ordered_${ordered_suite}_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
		uml_arm64_base_ordered_${ordered_suite}_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_base_ordered_${ordered_suite}_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_base_ordered_${ordered_suite}_tests PRIVATE
		uml_arm64_engine_test_support)
	add_test(NAME arm64-base-ordered-${ordered_suite}
		COMMAND uml_arm64_base_ordered_${ordered_suite}_tests)
    endforeach()

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
    target_link_libraries(uml_arm64_base_exclusive_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_exclusive_pair_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_compare_swap_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_compare_swap_pair_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_atomic_bit_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_atomic_minmax_tests PRIVATE
        uml_arm64_engine_test_support)
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
    target_link_libraries(uml_arm64_base_atomic_swap_tests PRIVATE
        uml_arm64_engine_test_support)
    add_test(NAME arm64-base-atomic-swap
		COMMAND uml_arm64_base_atomic_swap_tests)

    foreach(lse128_pair_suite IN ITEMS allocation behavior fault gadget)
	add_executable(uml_arm64_base_lse128_pair_${lse128_pair_suite}_tests
		tests/arm64_base_lse128_pair_${lse128_pair_suite}_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
		uml_arm64_base_lse128_pair_${lse128_pair_suite}_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_base_lse128_pair_${lse128_pair_suite}_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_base_lse128_pair_${lse128_pair_suite}_tests PRIVATE
		uml_arm64_engine_test_support)
	add_test(NAME arm64-base-lse128-pair-${lse128_pair_suite}
		COMMAND uml_arm64_base_lse128_pair_${lse128_pair_suite}_tests)
    endforeach()

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
	target_link_libraries(
		uml_arm64_atomic_floating_${atomic_floating_suite}_tests PRIVATE
		uml_arm64_engine_test_support)
	add_test(NAME arm64-atomic-floating-${atomic_floating_suite}
		COMMAND uml_arm64_atomic_floating_${atomic_floating_suite}_tests)
    endforeach()

    foreach(rcw_suite scalar pair allocation fault gadget)
	add_executable(uml_arm64_base_rcw_${rcw_suite}_tests
		tests/arm64_base_rcw_${rcw_suite}_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
		uml_arm64_base_rcw_${rcw_suite}_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_base_rcw_${rcw_suite}_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_base_rcw_${rcw_suite}_tests PRIVATE
		uml_arm64_engine_test_support)
	add_test(NAME arm64-base-rcw-${rcw_suite}
		COMMAND uml_arm64_base_rcw_${rcw_suite}_tests)
    endforeach()

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
    target_link_libraries(uml_arm64_base_clear_exclusive_tests PRIVATE
		uml_arm64_engine_test_support)
    add_test(NAME arm64-base-clear-exclusive
		COMMAND uml_arm64_base_clear_exclusive_tests)

    foreach(ordered_pair_suite IN ITEMS allocation behavior fault gadget)
	add_executable(uml_arm64_base_ordered_pair_${ordered_pair_suite}_tests
		tests/arm64_base_ordered_pair_${ordered_pair_suite}_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
		uml_arm64_base_ordered_pair_${ordered_pair_suite}_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_base_ordered_pair_${ordered_pair_suite}_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_base_ordered_pair_${ordered_pair_suite}_tests PRIVATE
		uml_arm64_engine_test_support)
	add_test(NAME arm64-base-ordered-pair-${ordered_pair_suite}
		COMMAND uml_arm64_base_ordered_pair_${ordered_pair_suite}_tests)
    endforeach()

    foreach(simd_ordinary_memory_suite IN ITEMS
		allocation immediate register pair fault tag gadget)
	add_executable(
		uml_arm64_simd_ordinary_memory_${simd_ordinary_memory_suite}_tests
		tests/arm64_simd_ordinary_memory_${simd_ordinary_memory_suite}_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
		uml_arm64_simd_ordinary_memory_${simd_ordinary_memory_suite}_tests
		PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_simd_ordinary_memory_${simd_ordinary_memory_suite}_tests
		PRIVATE -Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_simd_ordinary_memory_${simd_ordinary_memory_suite}_tests
		PRIVATE uml_arm64_engine_test_support)
	string(REPLACE "_" "-" simd_ordinary_memory_test_name
		"${simd_ordinary_memory_suite}")
	add_test(
		NAME arm64-simd-ordinary-memory-${simd_ordinary_memory_test_name}
		COMMAND
			uml_arm64_simd_ordinary_memory_${simd_ordinary_memory_suite}_tests)
    endforeach()

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
    target_link_libraries(uml_arm64_simd_load_store_pair_tests PRIVATE
        uml_arm64_engine_test_support)
    add_test(NAME arm64-simd-load-store-pair
		COMMAND uml_arm64_simd_load_store_pair_tests)

    foreach(simd_structure_suite IN ITEMS
		multiple_allocation
		multiple_behavior
		multiple_fault
		lane_allocation
		lane_behavior
		lane_fault
		replicate_allocation
		replicate_behavior
		replicate_fault
		store_atomicity
		tag
		gadget)
		add_executable(
			uml_arm64_simd_structure_${simd_structure_suite}_tests
			tests/arm64_simd_structure_${simd_structure_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_structure_${simd_structure_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_structure_${simd_structure_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_structure_${simd_structure_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		string(REPLACE "_" "-" simd_structure_test_name
			"${simd_structure_suite}")
		add_test(NAME arm64-simd-structure-${simd_structure_test_name}
			COMMAND
				uml_arm64_simd_structure_${simd_structure_suite}_tests)
    endforeach()

    foreach(simd_ordered_suite IN ITEMS allocation behavior fault gadget)
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
		target_link_libraries(
			uml_arm64_simd_ordered_${simd_ordered_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_three_same_${simd_three_same_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-simd-three-same-${simd_three_same_suite}
			COMMAND uml_arm64_simd_three_same_${simd_three_same_suite}_tests)
	endforeach()

	foreach(simd_widening_suite IN ITEMS allocation long wide absolute gadget)
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
		target_link_libraries(
			uml_arm64_simd_widening_${simd_widening_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-simd-widening-${simd_widening_suite}
			COMMAND uml_arm64_simd_widening_${simd_widening_suite}_tests)
	endforeach()

	foreach(simd_high_narrow_suite IN ITEMS
			allocation
			behavior
			rounding
			gadget)
		add_executable(
			uml_arm64_simd_high_narrow_${simd_high_narrow_suite}_tests
			tests/arm64_simd_high_narrow_${simd_high_narrow_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_simd_high_narrow_${simd_high_narrow_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_simd_high_narrow_${simd_high_narrow_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_simd_high_narrow_${simd_high_narrow_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-simd-high-narrow-${simd_high_narrow_suite}
			COMMAND
			uml_arm64_simd_high_narrow_${simd_high_narrow_suite}_tests)
	endforeach()

	foreach(simd_two_register_suite IN ITEMS
			allocation
			ordinary
			pairwise
			bit
			compare_zero
			narrow
			gadget)
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
		target_link_libraries(
			uml_arm64_simd_two_register_${simd_two_register_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_reduce_${simd_reduce_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_estimate_${simd_estimate_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-simd-estimate-${simd_estimate_suite}
			COMMAND uml_arm64_simd_estimate_${simd_estimate_suite}_tests)
	endforeach()

	foreach(simd_bitwise_suite IN ITEMS logic merge behavior gadget)
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
		target_link_libraries(
			uml_arm64_simd_bitwise_${simd_bitwise_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_immediate_${simd_immediate_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-simd-immediate-${simd_immediate_suite}
			COMMAND uml_arm64_simd_immediate_${simd_immediate_suite}_tests)
	endforeach()

	foreach(simd_rearrange_suite IN ITEMS
			allocation permute_behavior table_behavior gadget_structure)
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
		target_link_libraries(
			uml_arm64_simd_rearrange_${simd_rearrange_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-simd-rearrange-${simd_rearrange_suite}
			COMMAND uml_arm64_simd_rearrange_${simd_rearrange_suite}_tests)
	endforeach()

	foreach(simd_lut_suite IN ITEMS allocation behavior alias reserved gadget)
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
		target_link_libraries(
			uml_arm64_simd_lut_${simd_lut_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_basic_floating_${basic_floating_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_fused_floating_${fused_floating_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_floating_round_${floating_round_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-floating-round-${floating_round_suite}
			COMMAND uml_arm64_floating_round_${floating_round_suite}_tests)
	endforeach()

	foreach(integer_to_floating_suite IN ITEMS allocation behavior control
			rounding reserved gadget)
		add_executable(
			uml_arm64_integer_to_floating_${integer_to_floating_suite}_tests
			tests/arm64_integer_to_floating_${integer_to_floating_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_integer_to_floating_${integer_to_floating_suite}_tests
			PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_integer_to_floating_${integer_to_floating_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_integer_to_floating_${integer_to_floating_suite}_tests
			PRIVATE uml_arm64_engine_test_support)
		add_test(
			NAME arm64-integer-to-floating-${integer_to_floating_suite}
			COMMAND
			uml_arm64_integer_to_floating_${integer_to_floating_suite}_tests)
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
		target_link_libraries(
			uml_arm64_floating_horizontal_${floating_horizontal_suite}_tests
			PRIVATE uml_arm64_engine_test_support)
		add_test(NAME arm64-floating-horizontal-${floating_horizontal_suite}
			COMMAND
			uml_arm64_floating_horizontal_${floating_horizontal_suite}_tests)
	endforeach()

	foreach(floating_reciprocal_suite IN ITEMS allocation reserved estimate
			step control gadget)
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
		target_link_libraries(
			uml_arm64_floating_reciprocal_${floating_reciprocal_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_floating_complex_${floating_complex_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_floating_absolute_minmax_${floating_absolute_minmax_suite}_tests
			PRIVATE uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_floating_scale_${floating_scale_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_bfloat_conversion_${bfloat_conversion_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_bfloat_multiply_${bfloat_multiply_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_bfloat_dot_${bfloat_dot_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_bfloat_matrix_${bfloat_matrix_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-bfloat-matrix-${bfloat_matrix_suite}
			COMMAND uml_arm64_bfloat_matrix_${bfloat_matrix_suite}_tests)
	endforeach()

	foreach(half_fused_long_suite IN ITEMS allocation vector indexed control
			reserved gadget)
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
		target_link_libraries(
			uml_arm64_half_fused_long_${half_fused_long_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-half-fused-long-${half_fused_long_suite}
			COMMAND uml_arm64_half_fused_long_${half_fused_long_suite}_tests)
	endforeach()

	foreach(half_matrix_suite IN ITEMS allocation non_widening widening control
			reserved gadget)
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
		target_link_libraries(
			uml_arm64_half_matrix_${half_matrix_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-half-matrix-${half_matrix_suite}
			COMMAND uml_arm64_half_matrix_${half_matrix_suite}_tests)
	endforeach()

	foreach(half_dot_suite IN ITEMS allocation vector indexed control reserved
			gadget)
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
		target_link_libraries(uml_arm64_half_dot_${half_dot_suite}_tests
			PRIVATE uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_extract_${simd_extract_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-simd-extract-${simd_extract_suite}
			COMMAND uml_arm64_simd_extract_${simd_extract_suite}_tests)
	endforeach()

	foreach(simd_aes_suite IN ITEMS
			allocation behavior control reserved gadget)
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
		target_link_libraries(
			uml_arm64_simd_aes_${simd_aes_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-simd-aes-${simd_aes_suite}
			COMMAND uml_arm64_simd_aes_${simd_aes_suite}_tests)
	endforeach()

	foreach(simd_sha3_suite IN ITEMS allocation rotation_allocation behavior
			reserved gadget)
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
		target_link_libraries(
			uml_arm64_simd_sha3_${simd_sha3_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-simd-sha3-${simd_sha3_suite}
			COMMAND uml_arm64_simd_sha3_${simd_sha3_suite}_tests)
	endforeach()

	foreach(simd_pmull_suite IN ITEMS allocation matrix edge portable gadget)
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
		target_link_libraries(
			uml_arm64_simd_pmull_${simd_pmull_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_addp_scalar_${simd_addp_scalar_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_lane_move_${simd_lane_move_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_shift_immediate_${simd_shift_immediate_suite}_tests
			PRIVATE uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_shift_left_long_${simd_shift_left_long_suite}_tests
			PRIVATE uml_arm64_engine_test_support)
		add_test(NAME arm64-simd-shift-left-long-${simd_shift_left_long_suite}
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
		target_link_libraries(
			uml_arm64_simd_indexed_${simd_indexed_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-simd-indexed-${simd_indexed_suite}
			COMMAND uml_arm64_simd_indexed_${simd_indexed_suite}_tests)
	endforeach()

	foreach(simd_dot_suite IN ITEMS allocation vector indexed portable gadget)
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
		target_link_libraries(
			uml_arm64_simd_dot_${simd_dot_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_fp_indexed_${simd_fp_indexed_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-simd-fp-indexed-${simd_fp_indexed_suite}
			COMMAND
			uml_arm64_simd_fp_indexed_${simd_fp_indexed_suite}_tests)
	endforeach()

	foreach(simd_matrix_suite IN ITEMS allocation behavior registers portable
			gadget)
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
		target_link_libraries(
			uml_arm64_simd_matrix_${simd_matrix_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_fmulx_${simd_fmulx_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_fabs_fneg_${simd_fabs_fneg_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_fmov_${simd_fmov_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_fcsel_${simd_fcsel_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_fcmp_${simd_fcmp_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-simd-fcmp-${simd_fcmp_suite}
			COMMAND uml_arm64_simd_fcmp_${simd_fcmp_suite}_tests)
	endforeach()

	foreach(simd_fcvt_precision_suite IN ITEMS allocation exact rounding control
			gadget)
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
		target_link_libraries(
			uml_arm64_simd_fcvt_precision_${simd_fcvt_precision_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_floating_compare_${simd_floating_compare_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		target_compile_options(
			uml_arm64_simd_floating_compare_${simd_floating_compare_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		add_test(NAME arm64-simd-floating-compare-${simd_floating_compare_suite}
			COMMAND
			uml_arm64_simd_floating_compare_${simd_floating_compare_suite}_tests)
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
		target_link_libraries(
			uml_arm64_simd_fcvt_vector_${simd_fcvt_vector_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
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
		target_link_libraries(
			uml_arm64_simd_fcvtxn_${simd_fcvtxn_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		target_compile_options(uml_arm64_simd_fcvtxn_${simd_fcvtxn_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		add_test(NAME arm64-simd-fcvtxn-${simd_fcvtxn_suite}
			COMMAND uml_arm64_simd_fcvtxn_${simd_fcvtxn_suite}_tests)
	endforeach()

	foreach(floating_to_integer_suite IN ITEMS allocation behavior rounding
			fixed control)
		add_executable(
			uml_arm64_floating_to_integer_${floating_to_integer_suite}_tests
			tests/arm64_floating_to_integer_${floating_to_integer_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_floating_to_integer_${floating_to_integer_suite}_tests
			PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_floating_to_integer_${floating_to_integer_suite}_tests
			PRIVATE -Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_floating_to_integer_${floating_to_integer_suite}_tests
			PRIVATE uml_arm64_engine_test_support)
		add_test(NAME arm64-floating-to-integer-${floating_to_integer_suite}
			COMMAND
			uml_arm64_floating_to_integer_${floating_to_integer_suite}_tests)
	endforeach()

	add_executable(uml_arm64_floating_javascript_conversion_tests
		tests/arm64_floating_javascript_conversion_tests.cpp
		$<TARGET_OBJECTS:uml_arm64_dispatch>
		$<TARGET_OBJECTS:uml_static_mmu>
		$<TARGET_OBJECTS:uml_arm64_gadgets>)
	target_include_directories(
		uml_arm64_floating_javascript_conversion_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/darwin/include)
	target_compile_options(
		uml_arm64_floating_javascript_conversion_tests PRIVATE
		-Wall -Wextra -Werror)
	target_link_libraries(
		uml_arm64_floating_javascript_conversion_tests PRIVATE
		uml_arm64_engine_test_support)
	add_test(NAME arm64-floating-javascript-conversion
		COMMAND uml_arm64_floating_javascript_conversion_tests)

	foreach(simd_sha_suite IN ITEMS sha12_allocation sha1_behavior sha256_behavior
		sha512_allocation sha512_behavior sm_allocation sm3_behavior sm4_behavior
		fixed_transform_portable fixed_transform_gadget)
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
		target_link_libraries(
			uml_arm64_simd_${simd_sha_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-simd-${simd_sha_suite}
			COMMAND uml_arm64_simd_${simd_sha_suite}_tests)
	endforeach()

	foreach(fp8_multiply_matrix_suite IN ITEMS allocation multiply_behavior
		matrix_behavior scale control gadget)
		add_executable(
			uml_arm64_fp8_multiply_matrix_${fp8_multiply_matrix_suite}_tests
			tests/arm64_fp8_multiply_matrix_${fp8_multiply_matrix_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_fp8_multiply_matrix_${fp8_multiply_matrix_suite}_tests
			PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
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
		add_test(
			NAME arm64-fp8-multiply-matrix-${fp8_multiply_matrix_test_name}
			COMMAND
			uml_arm64_fp8_multiply_matrix_${fp8_multiply_matrix_suite}_tests)
	endforeach()

	foreach(fp8_dot_suite IN ITEMS allocation behavior scale control gadget)
		add_executable(uml_arm64_fp8_dot_${fp8_dot_suite}_tests
			tests/arm64_fp8_dot_${fp8_dot_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_fp8_dot_${fp8_dot_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_fp8_dot_${fp8_dot_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_fp8_dot_${fp8_dot_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-fp8-dot-${fp8_dot_suite}
			COMMAND uml_arm64_fp8_dot_${fp8_dot_suite}_tests)
	endforeach()

	foreach(fp8_convert_up_suite IN ITEMS allocation behavior rounding control
		gadget)
		add_executable(
			uml_arm64_fp8_convert_up_${fp8_convert_up_suite}_tests
			tests/arm64_fp8_convert_up_${fp8_convert_up_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_fp8_convert_up_${fp8_convert_up_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_fp8_convert_up_${fp8_convert_up_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_fp8_convert_up_${fp8_convert_up_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-fp8-convert-up-${fp8_convert_up_suite}
			COMMAND
			uml_arm64_fp8_convert_up_${fp8_convert_up_suite}_tests)
	endforeach()

	foreach(fp8_convert_down_suite IN ITEMS allocation behavior rounding control)
		add_executable(
			uml_arm64_fp8_convert_down_${fp8_convert_down_suite}_tests
			tests/arm64_fp8_convert_down_${fp8_convert_down_suite}_tests.cpp
			$<TARGET_OBJECTS:uml_arm64_dispatch>
			$<TARGET_OBJECTS:uml_static_mmu>
			$<TARGET_OBJECTS:uml_arm64_gadgets>)
		target_include_directories(
			uml_arm64_fp8_convert_down_${fp8_convert_down_suite}_tests PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/darwin/include)
		target_compile_options(
			uml_arm64_fp8_convert_down_${fp8_convert_down_suite}_tests PRIVATE
			-Wall -Wextra -Werror)
		target_link_libraries(
			uml_arm64_fp8_convert_down_${fp8_convert_down_suite}_tests PRIVATE
			uml_arm64_engine_test_support)
		add_test(NAME arm64-fp8-convert-down-${fp8_convert_down_suite}
			COMMAND
			uml_arm64_fp8_convert_down_${fp8_convert_down_suite}_tests)
	endforeach()
endif()

install(TARGETS uml_backend
    ARCHIVE DESTINATION lib
    PUBLIC_HEADER DESTINATION include/uml)
