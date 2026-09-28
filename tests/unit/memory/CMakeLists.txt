# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_memory_domain_lifetime_tests
    domain_lifetime_tests.cpp
    $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
target_link_libraries(uml_memory_domain_lifetime_tests PRIVATE
    UML::MemorySharedBacking)
target_include_directories(uml_memory_domain_lifetime_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin/memory
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_features(uml_memory_domain_lifetime_tests PRIVATE cxx_std_20)
target_compile_definitions(uml_memory_domain_lifetime_tests PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=1)
target_compile_options(uml_memory_domain_lifetime_tests PRIVATE
    -Wall -Wextra -Werror)
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

add_executable(uml_memory_address_window_tests
    address_window_tests.cpp
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu/address_layout.cpp)
target_link_libraries(uml_memory_address_window_tests PRIVATE
    UML::MemoryContract)
target_include_directories(uml_memory_address_window_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_features(uml_memory_address_window_tests PRIVATE cxx_std_20)
target_compile_options(uml_memory_address_window_tests PRIVATE
    -Wall -Wextra -Werror)
uml_add_unit_test(NAME memory-address-window-reads
    COMMAND uml_memory_address_window_tests)

add_executable(uml_execution_gate_profile_tests
    execution_gate_profile_tests.cpp
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu/execution_gate.cpp)
target_include_directories(uml_execution_gate_profile_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_definitions(uml_execution_gate_profile_tests PRIVATE
    UML_MEMORY_PROFILE_TESTING=1
    UML_RUNTIME_PROFILING=1)
target_compile_features(uml_execution_gate_profile_tests PRIVATE cxx_std_20)
target_compile_options(uml_execution_gate_profile_tests PRIVATE
    -Wall -Wextra -Werror)
uml_add_unit_test(NAME memory-execution-gate-profile
    COMMAND uml_execution_gate_profile_tests)

if(TARGET uml_native_mmu_test_variant)
    add_executable(uml_native_window_reservation_tests
        native_window_reservation_tests.cpp)
    target_link_libraries(uml_native_window_reservation_tests PRIVATE
        UML::MemoryTestComponent)
    # The native-window placement helper is shared with the ARM64 white-box
    # MMU tests and has no architecture content of its own.
    target_include_directories(uml_native_window_reservation_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
    target_compile_features(uml_native_window_reservation_tests PRIVATE
        cxx_std_20)
    target_compile_options(uml_native_window_reservation_tests PRIVATE
        -Wall -Wextra -Werror)
    # The test asserts where the host places allocations, which a concurrent
    # VM-heavy test can perturb.
    uml_add_unit_test(NAME memory-native-window-reservation
        COMMAND uml_native_window_reservation_tests)
    set_tests_properties(memory-native-window-reservation PROPERTIES
        RUN_SERIAL TRUE)
endif()

add_executable(uml_shared_backing_view_tests shared_backing_view_tests.cpp)
target_link_libraries(uml_shared_backing_view_tests PRIVATE
    UML::MemorySharedBacking)
target_compile_features(uml_shared_backing_view_tests PRIVATE cxx_std_20)
target_compile_options(uml_shared_backing_view_tests PRIVATE
    -Wall -Wextra -Werror)
uml_add_unit_test(NAME memory-shared-backing-view
    COMMAND uml_shared_backing_view_tests)

add_executable(uml_word_shared_mutex_tests word_shared_mutex_tests.cpp)
target_include_directories(uml_word_shared_mutex_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_features(uml_word_shared_mutex_tests PRIVATE cxx_std_20)
target_compile_options(uml_word_shared_mutex_tests PRIVATE
    -Wall -Wextra -Werror)
uml_add_unit_test(NAME memory-word-shared-mutex
    COMMAND uml_word_shared_mutex_tests LABELS tsan)

add_executable(uml_guest_ram_commitment_tests guest_ram_commitment_tests.cpp)
target_link_libraries(uml_guest_ram_commitment_tests PRIVATE
    UML::MemorySharedBacking)
target_compile_features(uml_guest_ram_commitment_tests PRIVATE cxx_std_20)
target_compile_options(uml_guest_ram_commitment_tests PRIVATE
    -Wall -Wextra -Werror)
# The footprint assertions read the whole process, which a concurrent
# memory-heavy test would move.
uml_add_unit_test(NAME memory-guest-ram-commitment
    COMMAND uml_guest_ram_commitment_tests)
set_tests_properties(memory-guest-ram-commitment PROPERTIES RUN_SERIAL TRUE)
