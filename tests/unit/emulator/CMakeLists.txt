# SPDX-License-Identifier: GPL-2.0-or-later

# Emulator product tests: options, the C API boundary, the run API, and
# native-window admission and ownership. Runs enter a test kernel image
# through the emulator test variant (darwin/emulator/src/testing.hpp). The
# native-window helpers are shared with the ARM64 white-box tests; every test
# here links the shared-backing archive MappedBacking references.

add_executable(uml_emulator_c_api_boundary_tests
    emulator_c_api_boundary_tests.cpp)
target_link_libraries(uml_emulator_c_api_boundary_tests PRIVATE
    UML::BridgeAbi
    uml_emulator_test_variant)
uml_add_unit_test(
    NAME emulator-c-api-boundary
    COMMAND uml_emulator_c_api_boundary_tests)

# The lifecycle calls' console requests, seen by a socket standing where the
# guest binds its console.
add_executable(uml_emulator_lifecycle_tests emulator_lifecycle_tests.cpp)
target_link_libraries(uml_emulator_lifecycle_tests PRIVATE
    UML::BridgeAbi
    UML::DeviceGuestConsole
    UML::MemorySharedBacking
    uml_emulator_test_variant)
target_include_directories(uml_emulator_lifecycle_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/runtime
    ${PROJECT_SOURCE_DIR}/tests/unit/mconsole)
uml_add_unit_test(
    NAME emulator-lifecycle
    COMMAND uml_emulator_lifecycle_tests
    LABELS console)

add_executable(uml_emulator_options_tests emulator_options_tests.cpp)
target_link_libraries(uml_emulator_options_tests PRIVATE
    UML::BridgeAbi
    UML::Emulator
    UML::MemorySharedBacking)
target_include_directories(uml_emulator_options_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/emulator/src)
uml_add_unit_test(NAME emulator-options COMMAND uml_emulator_options_tests)

add_executable(uml_emulator_api_tests emulator_api_tests.cpp)
target_link_libraries(uml_emulator_api_tests PRIVATE
    UML::BridgeAbi
    uml_emulator_test_variant
    UML::MemorySharedBacking
    uml_test_allocation_failure)
target_include_directories(uml_emulator_api_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
target_compile_definitions(uml_emulator_api_tests PRIVATE
    UML_EMULATOR_TEST_HAS_KERNEL_RUNNER=$<BOOL:${UML_BUILD_KERNEL_RUNNER}>
    UML_EMULATOR_TEST_HAS_SLIRP=$<BOOL:${UML_BUILD_NETWORK_PROVIDER}>)
if(TARGET uml_network_provider)
    target_link_libraries(uml_emulator_api_tests PRIVATE
        UML::NetworkProvider)
endif()
uml_add_unit_test(NAME emulator-api COMMAND uml_emulator_api_tests)

add_executable(uml_native_4k_admission_tests native_4k_admission_tests.cpp)
target_link_libraries(uml_native_4k_admission_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking
    uml_emulator_test_variant)
target_include_directories(uml_native_4k_admission_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu
    ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
uml_add_unit_test(
    NAME emulator-native-4k-admission
    COMMAND uml_native_4k_admission_tests
    LABELS tsan)
