# SPDX-License-Identifier: GPL-2.0-or-later

if(UML_BUILD_TOOLING_TESTS)
    # The CMake-only reset belongs to the unit suite and is registered only
    # when the tooling validators consume the evidence produced by unit tests.
    # A strict unit selection in that combined graph therefore clears stale
    # evidence before running any producer.
    uml_add_unit_test(
        NO_EVIDENCE_DEPENDENCY
        NAME arm64-instruction-evidence-reset
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_ROOT=${CMAKE_BINARY_DIR}
            -P ${PROJECT_SOURCE_DIR}/tests/unit/arm64/reset_instruction_evidence.cmake
        LABELS cpu;audit)
endif()

add_library(uml_test_allocation_failure STATIC allocation_failure.cpp)
target_include_directories(uml_test_allocation_failure PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_test_allocation_failure PUBLIC cxx_std_20)
target_compile_options(uml_test_allocation_failure PRIVATE
    -Wall -Wextra -Werror)

# CPU conformance and engine/runtime tests have their own directory scope.
# This keeps the suite entry point small while preserving one explicit source
# manifest per test domain.
add_subdirectory(arm64)
add_subdirectory(memory)
add_subdirectory(runtime)

add_executable(uml_emulator_c_api_boundary_tests
    emulator_c_api_boundary_tests.cpp)
target_link_libraries(uml_emulator_c_api_boundary_tests PRIVATE UML::Emulator)
target_include_directories(uml_emulator_c_api_boundary_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include)
target_compile_features(uml_emulator_c_api_boundary_tests PRIVATE cxx_std_20)
target_compile_options(uml_emulator_c_api_boundary_tests PRIVATE
    -Wall -Wextra -Werror)
add_dependencies(unit-test-binaries uml_emulator_c_api_boundary_tests)
uml_add_unit_test(
    NAME emulator-c-api-boundary
    COMMAND uml_emulator_c_api_boundary_tests)

add_subdirectory(gpu)

get_property(_uml_registered_unit_binary_targets GLOBAL PROPERTY
    UML_UNIT_TEST_BINARY_TARGETS)
if(_uml_registered_unit_binary_targets)
    list(REMOVE_DUPLICATES _uml_registered_unit_binary_targets)
    add_dependencies(unit-test-binaries
        uml_instruction_evidence_test_runner
        ${_uml_registered_unit_binary_targets})
endif()
