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
add_dependencies(unit-test-binaries uml_memory_domain_lifetime_tests)

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
add_dependencies(unit-test-binaries uml_execution_gate_profile_tests)

add_executable(uml_shared_backing_view_tests shared_backing_view_tests.cpp)
target_link_libraries(uml_shared_backing_view_tests PRIVATE
    UML::MemorySharedBacking)
target_compile_features(uml_shared_backing_view_tests PRIVATE cxx_std_20)
target_compile_options(uml_shared_backing_view_tests PRIVATE
    -Wall -Wextra -Werror)
uml_add_unit_test(NAME memory-shared-backing-view
    COMMAND uml_shared_backing_view_tests)
add_dependencies(unit-test-binaries uml_shared_backing_view_tests)
