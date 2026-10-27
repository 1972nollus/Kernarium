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

# The ledger of what an embedded kernel creates through its imports.
add_executable(uml_runtime_kernel_resources_tests kernel_resources_tests.cpp)
target_link_libraries(uml_runtime_kernel_resources_tests PRIVATE
    UML::BridgeAbi
    uml_emulator_test_variant)
target_include_directories(uml_runtime_kernel_resources_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/runtime)
uml_add_unit_test(
    NAME runtime-kernel-resources
    COMMAND uml_runtime_kernel_resources_tests)

# The loader's release of the kernel image against a host-service view that
# still borrows its guest-RAM tail.
add_executable(uml_runtime_kernel_image_release_tests
    kernel_image_release_tests.cpp)
target_link_libraries(uml_runtime_kernel_image_release_tests PRIVATE
    UML::RuntimeKernelSignals)
uml_add_unit_test(
    NAME runtime-kernel-image-release
    COMMAND uml_runtime_kernel_image_release_tests
    LABELS tsan)

add_executable(uml_runtime_sequential_run_tests sequential_run_tests.cpp)
target_link_libraries(uml_runtime_sequential_run_tests PRIVATE
    UML::BridgeAbi
    UML::DeviceSocketPaths
    UML::MemorySharedBacking
    uml_emulator_test_variant
    uml_test_mach_vm_region)
# The JIT's exclusive-monitor probe the restart modes read between runs.
target_sources(uml_runtime_sequential_run_tests PRIVATE
    $<TARGET_OBJECTS:uml_arm64_jit_test_support>)
target_include_directories(uml_runtime_sequential_run_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/platform
    ${PROJECT_SOURCE_DIR}/darwin/runtime
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
# A guest that restarts itself boots again in the same process; every other end
# leaves the guest spent, and the hand-back refuses what still holds the guest.
# One process each, because the claim on guest RAM outlives every handle.
foreach(_uml_restart_mode IN ITEMS restart power-off halt stopped failed
        another-handle borrowed-view running-thread)
    uml_add_unit_test(
        NAME runtime-restart-${_uml_restart_mode}
        COMMAND uml_runtime_sequential_run_tests ${_uml_restart_mode})
endforeach()
unset(_uml_restart_mode)

add_executable(uml_runtime_x86_tier_tests x86_tier_tests.cpp)
target_include_directories(uml_runtime_x86_tier_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME runtime-x86-tier
    COMMAND uml_runtime_x86_tier_tests
    LABELS tsan)

# The registry of an embedded kernel's threads across a storm of restarts.
add_executable(uml_runtime_kernel_threads_tests kernel_threads_tests.cpp)
target_link_libraries(uml_runtime_kernel_threads_tests PRIVATE
    UML::RuntimeThreadFence)
uml_add_unit_test(
    NAME runtime-kernel-threads
    COMMAND uml_runtime_kernel_threads_tests)

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
