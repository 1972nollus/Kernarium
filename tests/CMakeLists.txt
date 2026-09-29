# SPDX-License-Identifier: GPL-2.0-or-later

# The standalone GPU server and VNC client are host test fixtures. Build them
# when either their tooling smoke or the external integration workflows own a
# use, without making either suite depend on the other suite being enabled.
set(_uml_need_vnc_fixtures OFF)
if(UML_BUILD_TOOLING_TESTS OR
   (UML_BUILD_INTEGRATION_TESTS AND UML_ENABLE_EXTERNAL_NETWORK_TESTS))
    set(_uml_need_vnc_fixtures ON)
endif()
if(_uml_need_vnc_fixtures AND TARGET uml_gpu_service)
    add_executable(uml-virtio-gpu
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/virtio_gpu_fixture.cpp)
    target_link_libraries(uml-virtio-gpu PRIVATE UML::GpuDevice)
    target_include_directories(uml-virtio-gpu PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    target_compile_options(uml-virtio-gpu PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    set_target_properties(uml-virtio-gpu PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

    add_executable(uml-vnc-smoke-client
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/vnc_smoke_client.cpp)
    target_link_libraries(uml-vnc-smoke-client PRIVATE
        UML::ThirdPartyVncClient
        "-framework CoreFoundation")
    target_compile_options(uml-vnc-smoke-client PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    set_target_properties(uml-vnc-smoke-client PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()

# Manual benchmark payloads have explicit source ownership but remain outside
# all correctness-suite aggregates and registrations.
add_subdirectory(perf EXCLUDE_FROM_ALL)

if(UML_BUILD_UNIT_TESTS)
    add_library(uml_host_capabilities_test_support OBJECT
        unit/host_capabilities_test_support.cpp)
    target_link_libraries(uml_host_capabilities_test_support PRIVATE
        UML::CpuContract
        UML::PlatformContract)
    target_include_directories(uml_host_capabilities_test_support PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_options(uml_host_capabilities_test_support PRIVATE
        -Wall -Wextra -Werror)
    # Unit tests keep scratch files in the per-test build-tree directory that
    # uml_add_unit_test() names, instead of a host-global path.
    add_library(uml_test_scratch_directory OBJECT
        unit/scratch/scratch_directory.cpp)
    target_include_directories(uml_test_scratch_directory PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/scratch)
    target_compile_options(uml_test_scratch_directory PRIVATE
        -Wall -Wextra -Werror)
    # One Mach VM region query for every test that inspects this task's map.
    add_library(uml_test_mach_vm_region INTERFACE)
    target_include_directories(uml_test_mach_vm_region INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/mach_vm)
    # Change-sequence reads the tests make without an execution context. An
    # archive, so a binary that names neither helper links no MMU symbols.
    add_library(uml_static_mmu_test_support STATIC
        unit/static_mmu/static_mmu_test_support.cpp)
    target_link_libraries(uml_static_mmu_test_support PRIVATE UML::BridgeAbi)
    target_include_directories(uml_static_mmu_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/static_mmu)
    target_compile_options(uml_static_mmu_test_support PRIVATE
        -Wall -Wextra -Werror)
    # Unit binaries are independent consumers of the same scoped build
    # configuration as production targets; no directory-global definitions
    # are needed.
    link_libraries(
        uml_host_capabilities_test_support
        uml_test_scratch_directory
        uml_test_mach_vm_region
        uml_static_mmu_test_support
        UML::GadgetHistogramConfiguration
        UML::GuestPageConfiguration
        UML::JitProfileConfiguration)
    add_subdirectory(unit)
endif()

if(UML_BUILD_TOOLING_TESTS)
    add_subdirectory(tooling)
endif()

if(UML_BUILD_INTEGRATION_TESTS)
    # CTest commands reference these host fixtures directly. Keep their build
    # ownership explicit so the integration preset produces every executable
    # required by the registered suite, not only the emulator app.
    add_custom_target(integration-test-binaries)
    add_subdirectory(integration)
    set(_integration_targets uml-darwin)
    if(UML_ENABLE_EXTERNAL_NETWORK_TESTS)
        list(APPEND _integration_targets
            uml-virtio-gpu uml-vnc-smoke-client)
    endif()
    foreach(_integration_target IN LISTS _integration_targets)
        if(NOT TARGET ${_integration_target})
            message(FATAL_ERROR
                "Integration target is not available: ${_integration_target}")
        endif()
        add_dependencies(integration-test-binaries ${_integration_target})
    endforeach()
endif()

# Suite aggregates are derived from the actual target-backed CTest commands,
# including commands whose fixtures are owned by another directory. A build
# preset therefore cannot finish successfully while leaving a registered test
# executable absent from a fresh graph.
if(UML_BUILD_UNIT_TESTS)
    uml_finalize_suite_binary_target(unit unit-test-binaries)
endif()
if(UML_BUILD_UNIT_TESTS AND UML_BUILD_GPU)
    uml_finalize_test_preset_binary_target(unit-gpu gpu-unit-test-binaries)
endif()
if(UML_BUILD_TOOLING_TESTS)
    uml_finalize_suite_binary_target(tooling tooling-test-binaries)
endif()
if(UML_BUILD_INTEGRATION_TESTS)
    uml_finalize_suite_binary_target(integration integration-test-binaries)
endif()
