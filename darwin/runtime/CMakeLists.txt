# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_runtime_contract INTERFACE)
add_library(UML::RuntimeContract ALIAS uml_runtime_contract)
uml_declare_architecture_target(uml_runtime_contract runtime)
target_link_libraries(uml_runtime_contract INTERFACE
    UML::BridgeAbi
    UML::CpuContract
    UML::MemoryContract
    UML::PlatformContract)

add_library(uml_static_runtime OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/static_guest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_gadget_runtime.cpp)
uml_declare_architecture_target(uml_static_runtime runtime)
target_link_libraries(uml_static_runtime PRIVATE
    UML::RuntimeContract
    uml_cpu_engine_factory
    uml_static_guest_memory)
target_include_directories(uml_static_runtime PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_definitions(uml_static_runtime PRIVATE
    UML_RUNTIME_PROFILING=$<BOOL:${UML_ENABLE_RUNTIME_PROFILING}>)

add_library(uml_runtime_context OBJECT
    ${PROJECT_SOURCE_DIR}/darwin/runtime/context.cpp
    ${PROJECT_SOURCE_DIR}/darwin/runtime/native_kernel_layout.cpp)
uml_declare_architecture_target(uml_runtime_context runtime)
target_link_libraries(uml_runtime_context PRIVATE
    UML::RuntimeContract
    UML::RunRouting
    UML::HostEnvironment
    uml_cpu_engine_factory
    uml_static_guest_memory)
target_include_directories(uml_runtime_context PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
# The Simulator can publish into a MAP_JIT arena and so could default to the
# JIT, but measurement rejects it for the workloads that matter: see G157.
target_compile_definitions(uml_runtime_context PRIVATE
    UML_DEFAULT_EXECUTION_ENGINE_JIT=$<BOOL:${UML_TARGET_MACOS}>)

# Process signal state an embedded kernel image leaves behind; the loader
# closes this scope before it releases the image.
add_library(uml_runtime_kernel_signals OBJECT
    ${PROJECT_SOURCE_DIR}/darwin/runtime/kernel_signal_scope.cpp)
add_library(UML::RuntimeKernelSignals ALIAS uml_runtime_kernel_signals)
uml_declare_architecture_target(uml_runtime_kernel_signals runtime)
target_include_directories(uml_runtime_kernel_signals PUBLIC
    ${PROJECT_SOURCE_DIR}/darwin/runtime)

add_library(uml_runtime_guest_selector OBJECT
    ${PROJECT_SOURCE_DIR}/darwin/runtime/guest_selector.cpp)
uml_declare_architecture_target(uml_runtime_guest_selector runtime)
target_link_libraries(uml_runtime_guest_selector PRIVATE
    UML::RuntimeContract
    uml_cpu_engine_factory
    uml_static_guest_memory)
target_include_directories(uml_runtime_guest_selector PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)

add_library(uml_runtime_entry OBJECT
    ${PROJECT_SOURCE_DIR}/darwin/runtime/bridge.cpp
    ${PROJECT_SOURCE_DIR}/darwin/runtime/guest_sync.cpp)
uml_declare_architecture_target(uml_runtime_entry runtime)
target_link_libraries(uml_runtime_entry PRIVATE
    UML::DeviceSocketPaths
    UML::GuestFiles
    UML::MemorySharedBacking
    UML::RuntimeContract
    uml_cpu_engine_factory
    uml_static_guest_memory)
target_include_directories(uml_runtime_entry PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/platform)

add_library(uml_runtime_objects INTERFACE)
add_library(UML::RuntimeObjects ALIAS uml_runtime_objects)
uml_declare_architecture_target(uml_runtime_objects runtime)
target_link_libraries(uml_runtime_objects INTERFACE UML::RuntimeContract)
target_sources(uml_runtime_objects INTERFACE
	$<TARGET_OBJECTS:uml_runtime_context>
    $<TARGET_OBJECTS:uml_runtime_kernel_signals>
    $<TARGET_OBJECTS:uml_runtime_guest_selector>
    $<TARGET_OBJECTS:uml_runtime_entry>
    $<TARGET_OBJECTS:uml_static_runtime>)

# This composition target consumes module-owned object carriers without an
# unused intermediate archive.
add_library(uml_runtime_core INTERFACE)
add_library(UML::RuntimeCore ALIAS uml_runtime_core)
uml_declare_architecture_target(uml_runtime_core runtime)
target_link_libraries(uml_runtime_core INTERFACE
    UML::BridgeAbi
    UML::RuntimeObjects
    uml_cpu_engine_component
    UML::PlatformComponent)
target_include_directories(uml_runtime_core INTERFACE
    ${PROJECT_SOURCE_DIR}/darwin)

if(TARGET uml_memory_test_component)
    add_library(uml_runtime_core_test_variant INTERFACE)
    add_library(UML::RuntimeCoreTestVariant ALIAS
        uml_runtime_core_test_variant)
    uml_declare_architecture_target(uml_runtime_core_test_variant runtime)
    target_link_libraries(uml_runtime_core_test_variant INTERFACE
        UML::BridgeAbi
        UML::RuntimeObjects
        UML::CpuEngineObjects
        UML::MemoryTestComponent
        UML::PlatformComponent)
    target_include_directories(uml_runtime_core_test_variant INTERFACE
        ${PROJECT_SOURCE_DIR}/darwin)
endif()
