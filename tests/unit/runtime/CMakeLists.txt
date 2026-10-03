# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_runtime_context_tests context_tests.cpp)
target_link_libraries(uml_runtime_context_tests PRIVATE
    UML::BridgeAbi
    UML::Emulator
    uml_test_allocation_failure)
target_include_directories(uml_runtime_context_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME runtime-context-lifecycle
    COMMAND uml_runtime_context_tests
    LABELS tsan)

add_executable(uml_runtime_exit_order_tests exit_order_tests.cpp)
target_link_libraries(uml_runtime_exit_order_tests PRIVATE
    UML::BridgeAbi
    UML::Emulator)
target_include_directories(uml_runtime_exit_order_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME runtime-exit-order
    COMMAND uml_runtime_exit_order_tests)

add_executable(uml_runtime_kernel_signal_scope_tests
    kernel_signal_scope_tests.cpp)
target_link_libraries(uml_runtime_kernel_signal_scope_tests PRIVATE
    UML::RuntimeKernelSignals)
uml_add_unit_test(
    NAME runtime-kernel-signal-scope
    COMMAND uml_runtime_kernel_signal_scope_tests)

add_executable(uml_runtime_sequential_run_tests sequential_run_tests.cpp)
target_link_libraries(uml_runtime_sequential_run_tests PRIVATE
    UML::BridgeAbi
    uml_emulator_test_variant)
uml_add_unit_test(
    NAME runtime-sequential-run-same-handle
    COMMAND uml_runtime_sequential_run_tests same-handle)
uml_add_unit_test(
    NAME runtime-sequential-run-fresh-handle
    COMMAND uml_runtime_sequential_run_tests fresh-handle)
uml_add_unit_test(
    NAME runtime-sequential-run-unclaimed-first-run
    COMMAND uml_runtime_sequential_run_tests unclaimed-first-run)
