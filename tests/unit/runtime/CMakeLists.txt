# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_run_routing_test_variant OBJECT
    ${PROJECT_SOURCE_DIR}/darwin/platform/run_routing.cpp)
target_include_directories(uml_run_routing_test_variant PUBLIC
    ${PROJECT_SOURCE_DIR}/darwin/platform)
target_compile_definitions(uml_run_routing_test_variant PRIVATE
    UML_RUN_ROUTING_TESTING=1)
target_compile_features(uml_run_routing_test_variant PUBLIC cxx_std_20)
target_compile_options(uml_run_routing_test_variant PRIVATE
    -Wall -Wextra -Werror)

add_executable(uml_run_routing_tests
    run_routing_tests.cpp
    $<TARGET_OBJECTS:uml_run_routing_test_variant>)
target_link_libraries(uml_run_routing_tests PRIVATE
    uml_test_allocation_failure)
target_include_directories(uml_run_routing_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform)
target_compile_definitions(uml_run_routing_tests PRIVATE
    UML_RUN_ROUTING_TESTING=1)
target_compile_features(uml_run_routing_tests PRIVATE cxx_std_20)
target_compile_options(uml_run_routing_tests PRIVATE
    -Wall -Wextra -Werror)
add_dependencies(unit-test-binaries uml_run_routing_tests)
uml_add_unit_test(
    NAME runtime-route-transaction
    COMMAND uml_run_routing_tests
    LABELS tsan)

add_executable(uml_runtime_context_tests context_tests.cpp)
target_link_libraries(uml_runtime_context_tests PRIVATE
    UML::Emulator
    uml_test_allocation_failure)
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

add_executable(uml_runtime_exit_order_tests exit_order_tests.cpp)
target_link_libraries(uml_runtime_exit_order_tests PRIVATE UML::Emulator)
target_include_directories(uml_runtime_exit_order_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/include)
target_compile_features(uml_runtime_exit_order_tests PRIVATE cxx_std_20)
target_compile_options(uml_runtime_exit_order_tests PRIVATE
    -Wall -Wextra -Werror)
add_dependencies(unit-test-binaries uml_runtime_exit_order_tests)
uml_add_unit_test(
    NAME runtime-exit-order
    COMMAND uml_runtime_exit_order_tests)

add_executable(uml_runtime_kernel_signal_scope_tests
    kernel_signal_scope_tests.cpp)
target_link_libraries(uml_runtime_kernel_signal_scope_tests PRIVATE
    UML::RuntimeKernelSignals)
target_compile_features(uml_runtime_kernel_signal_scope_tests PRIVATE
    cxx_std_20)
target_compile_options(uml_runtime_kernel_signal_scope_tests PRIVATE
    -Wall -Wextra -Werror)
add_dependencies(unit-test-binaries uml_runtime_kernel_signal_scope_tests)
uml_add_unit_test(
    NAME runtime-kernel-signal-scope
    COMMAND uml_runtime_kernel_signal_scope_tests)

add_executable(uml_runtime_sequential_run_tests sequential_run_tests.cpp)
target_link_libraries(uml_runtime_sequential_run_tests PRIVATE UML::Emulator)
target_include_directories(uml_runtime_sequential_run_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include)
target_compile_features(uml_runtime_sequential_run_tests PRIVATE cxx_std_20)
target_compile_options(uml_runtime_sequential_run_tests PRIVATE
    -Wall -Wextra -Werror)
add_dependencies(unit-test-binaries uml_runtime_sequential_run_tests)
uml_add_unit_test(
    NAME runtime-sequential-run-same-handle
    COMMAND uml_runtime_sequential_run_tests same-handle)
uml_add_unit_test(
    NAME runtime-sequential-run-fresh-handle
    COMMAND uml_runtime_sequential_run_tests fresh-handle)
uml_add_unit_test(
    NAME runtime-sequential-run-unclaimed-first-run
    COMMAND uml_runtime_sequential_run_tests unclaimed-first-run)
