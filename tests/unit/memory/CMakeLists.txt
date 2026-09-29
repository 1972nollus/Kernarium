# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_memory_domain_lifetime_tests
    domain_lifetime_tests.cpp
    $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
target_link_libraries(uml_memory_domain_lifetime_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking)
target_include_directories(uml_memory_domain_lifetime_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_definitions(uml_memory_domain_lifetime_tests PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=1)
if(TARGET uml_native_mmu_test_variant)
    target_sources(uml_memory_domain_lifetime_tests PRIVATE
        $<TARGET_OBJECTS:uml_native_mmu_test_variant>)
    target_compile_definitions(uml_memory_domain_lifetime_tests PRIVATE
        UML_MEMORY_DOMAIN_TEST_HAS_NATIVE=1)
else()
    target_compile_definitions(uml_memory_domain_lifetime_tests PRIVATE
        UML_MEMORY_DOMAIN_TEST_HAS_NATIVE=0)
endif()
uml_add_unit_test(NAME memory-domain-lifetime
    COMMAND uml_memory_domain_lifetime_tests LABELS tsan)

add_executable(uml_memory_address_window_tests address_window_tests.cpp)
target_link_libraries(uml_memory_address_window_tests PRIVATE
    uml_address_layout_test_variant)
uml_add_unit_test(NAME memory-address-window-reads
    COMMAND uml_memory_address_window_tests)

add_executable(uml_execution_gate_profile_tests
    execution_gate_profile_tests.cpp)
target_link_libraries(uml_execution_gate_profile_tests PRIVATE
    UML::BridgeAbi
    uml_execution_gate_profile_test_variant)
uml_add_unit_test(NAME memory-execution-gate-profile
    COMMAND uml_execution_gate_profile_tests)

if(TARGET uml_native_mmu_test_variant)
    add_executable(uml_native_window_reservation_tests
        native_window_reservation_tests.cpp)
    target_link_libraries(uml_native_window_reservation_tests PRIVATE
        UML::BridgeAbi
        UML::MemoryTestComponent)
    # The native-window placement helper is shared with the ARM64 white-box
    # MMU tests and has no architecture content of its own.
    target_include_directories(uml_native_window_reservation_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
    # The test asserts where the host places allocations, which a concurrent
    # VM-heavy test can perturb.
    uml_add_unit_test(NAME memory-native-window-reservation
        COMMAND uml_native_window_reservation_tests)
    # Unsupported beside AddressSanitizer's heap
    # (tests/unit/arm64/native_window_test_support.hpp).
    set_tests_properties(memory-native-window-reservation PROPERTIES
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77)
endif()

# The direct-to-shadow slot conversion exists only where a guest page is
# smaller than the host page.
if(TARGET uml_native_mmu_test_variant AND NOT UML_GUEST_16K_PAGES)
    add_executable(uml_native_direct_conversion_tests
        native_direct_conversion_tests.cpp)
    target_link_libraries(uml_native_direct_conversion_tests PRIVATE
        UML::BridgeAbi
        UML::MemoryTestComponent)
    target_include_directories(uml_native_direct_conversion_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
    # Like the other native-window tests, it maps at fixed window addresses
    # that a concurrent VM-heavy test can occupy.
    uml_add_unit_test(NAME memory-native-direct-conversion
        COMMAND uml_native_direct_conversion_tests)
    set_tests_properties(memory-native-direct-conversion PROPERTIES
        RUN_SERIAL TRUE)
endif()

add_executable(uml_shared_backing_view_tests shared_backing_view_tests.cpp)
target_link_libraries(uml_shared_backing_view_tests PRIVATE
    UML::MemorySharedBacking)
uml_add_unit_test(NAME memory-shared-backing-view
    COMMAND uml_shared_backing_view_tests)

add_executable(uml_word_shared_mutex_tests word_shared_mutex_tests.cpp)
target_include_directories(uml_word_shared_mutex_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
uml_add_unit_test(NAME memory-word-shared-mutex
    COMMAND uml_word_shared_mutex_tests LABELS tsan)

add_executable(uml_guest_ram_commitment_tests guest_ram_commitment_tests.cpp)
target_link_libraries(uml_guest_ram_commitment_tests PRIVATE
    UML::MemorySharedBacking)
# The footprint assertions read the whole process, which a concurrent
# memory-heavy test would move.
uml_add_unit_test(NAME memory-guest-ram-commitment
    COMMAND uml_guest_ram_commitment_tests)
set_tests_properties(memory-guest-ram-commitment PROPERTIES RUN_SERIAL TRUE)

# Raw MMU compositions carry the shared-backing archive MappedBacking
# references, as the production memory component does.
add_executable(uml_static_mmu_batch_tests
    static_mmu_batch_tests.cpp
    $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
target_link_libraries(uml_static_mmu_batch_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking)
target_include_directories(uml_static_mmu_batch_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_definitions(uml_static_mmu_batch_tests PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=1)
uml_add_unit_test(NAME static-mmu-batch COMMAND uml_static_mmu_batch_tests
    LABELS tsan)

if(NOT UML_GUEST_16K_PAGES)
    add_executable(uml_native_mmu_tests
        native_mmu_tests.cpp
        $<TARGET_OBJECTS:uml_native_mmu_test_variant>
        $<TARGET_OBJECTS:uml_memory_vm_bridge>
        $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
    target_link_libraries(uml_native_mmu_tests PRIVATE
        UML::BridgeAbi
        UML::MemorySharedBacking)
    target_include_directories(uml_native_mmu_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
    target_compile_definitions(uml_native_mmu_tests PRIVATE
        UML_MEMORY_ALLOCATION_TESTING=1)
    uml_add_unit_test(NAME native-4k-shadow-mmu COMMAND uml_native_mmu_tests)
    # The test reserves the fixed Darwin native bridge window. Running after
    # several concurrent VM-heavy tests can make that host reservation fail
    # transiently even though an isolated retry succeeds.
    set_tests_properties(native-4k-shadow-mmu PROPERTIES RUN_SERIAL TRUE)
endif()
