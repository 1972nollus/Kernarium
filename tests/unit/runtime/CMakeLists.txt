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
target_include_directories(uml_runtime_sequential_run_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/unit/emulator)
uml_add_unit_test(
    NAME runtime-sequential-run-same-handle
    COMMAND uml_runtime_sequential_run_tests same-handle)
uml_add_unit_test(
    NAME runtime-sequential-run-fresh-handle
    COMMAND uml_runtime_sequential_run_tests fresh-handle)
uml_add_unit_test(
    NAME runtime-sequential-run-unclaimed-first-run
    COMMAND uml_runtime_sequential_run_tests unclaimed-first-run)

add_executable(uml_runtime_x86_tier_tests x86_tier_tests.cpp)
target_include_directories(uml_runtime_x86_tier_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME runtime-x86-tier
    COMMAND uml_runtime_x86_tier_tests
    LABELS tsan)

# The fence against real threads. Each test stops and starts threads of the
# process, so the binaries run alone.
add_executable(uml_runtime_thread_fence_tests thread_fence_tests.cpp)
target_link_libraries(uml_runtime_thread_fence_tests PRIVATE
    UML::RuntimeThreadFence)
uml_add_unit_test(
    NAME runtime-thread-fence
    COMMAND uml_runtime_thread_fence_tests
    TIMEOUT 240)
uml_add_unit_test(
    NAME runtime-thread-fence-registry-overflow
    COMMAND uml_runtime_thread_fence_tests overflow
    TIMEOUT 120)
set_tests_properties(runtime-thread-fence
    runtime-thread-fence-registry-overflow PROPERTIES RUN_SERIAL TRUE)

# The order of a hibernation against recording steps, and a real one against
# real kernel threads and guest RAM.
add_executable(uml_runtime_guest_hibernation_tests guest_hibernation_tests.cpp)
target_link_libraries(uml_runtime_guest_hibernation_tests PRIVATE
    UML::RuntimeHibernation
    UML::RuntimeThreadFence
    UML::MemorySharedBacking)
uml_add_unit_test(
    NAME runtime-guest-hibernation
    COMMAND uml_runtime_guest_hibernation_tests
    TIMEOUT 240)
set_tests_properties(runtime-guest-hibernation PROPERTIES RUN_SERIAL TRUE)
