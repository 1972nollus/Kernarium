# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_runtime_context_tests context_tests.cpp)
target_link_libraries(uml_runtime_context_tests PRIVATE UML::Emulator)
target_include_directories(uml_runtime_context_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/include)
target_compile_features(uml_runtime_context_tests PRIVATE cxx_std_20)
target_compile_options(uml_runtime_context_tests PRIVATE
    -Wall -Wextra -Werror)
add_dependencies(unit-test-binaries uml_runtime_context_tests)
uml_add_unit_test(
    NAME runtime-context-lifecycle
    COMMAND uml_runtime_context_tests
    LABELS tsan)
