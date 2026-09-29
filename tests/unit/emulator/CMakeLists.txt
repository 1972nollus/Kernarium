# SPDX-License-Identifier: GPL-2.0-or-later

# Emulator product tests: options, the C API boundary, the native entry API,
# and native-window admission and ownership through the admission variant.
# The native-window helpers are shared with the ARM64 white-box tests; every
# test here links the shared-backing archive MappedBacking references.

add_executable(uml_emulator_c_api_boundary_tests
    emulator_c_api_boundary_tests.cpp)
target_link_libraries(uml_emulator_c_api_boundary_tests PRIVATE
    UML::BridgeAbi
    UML::Emulator)
uml_add_unit_test(
    NAME emulator-c-api-boundary
    COMMAND uml_emulator_c_api_boundary_tests)

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
    UML::Emulator
    UML::MemorySharedBacking
    uml_test_allocation_failure)
target_include_directories(uml_emulator_api_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
target_compile_definitions(uml_emulator_api_tests PRIVATE
    UML_EMULATOR_TEST_HAS_SLIRP=$<BOOL:${UML_BUILD_NETWORK_PROVIDER}>)
if(TARGET uml_network_provider)
    target_link_libraries(uml_emulator_api_tests PRIVATE
        UML::NetworkProvider)
endif()
uml_add_unit_test(NAME emulator-native-entry-api COMMAND uml_emulator_api_tests)

if(NOT UML_GUEST_16K_PAGES)
    add_executable(uml_native_4k_admission_tests native_4k_admission_tests.cpp)
    target_link_libraries(uml_native_4k_admission_tests PRIVATE
        UML::BridgeAbi
        UML::MemorySharedBacking
        uml_emulator_admission_test_variant)
    target_include_directories(uml_native_4k_admission_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/emulator/src
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
    target_compile_definitions(uml_native_4k_admission_tests PRIVATE
        UML_EMULATOR_ADMISSION_TESTING=1)
    uml_add_unit_test(
        NAME emulator-native-4k-admission
        COMMAND uml_native_4k_admission_tests
        LABELS tsan)
endif()

if(UML_GUEST_16K_PAGES AND UML_TARGET_MACOS)
    add_executable(uml_native_16k_ownership_tests
        native_16k_ownership_tests.cpp)
    target_link_libraries(uml_native_16k_ownership_tests PRIVATE
        UML::BridgeAbi
        UML::MemorySharedBacking
        uml_emulator_admission_test_variant)
    target_include_directories(uml_native_16k_ownership_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
    uml_add_unit_test(
        NAME emulator-native-16k-ownership
        COMMAND uml_native_16k_ownership_tests
        LABELS tsan)
    # Unsupported beside AddressSanitizer's heap (native_window_test_support.hpp).
    set_tests_properties(emulator-native-16k-ownership PROPERTIES
        SKIP_RETURN_CODE 77)
endif()
