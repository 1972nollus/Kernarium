# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_memory_contract INTERFACE)
add_library(UML::MemoryContract ALIAS uml_memory_contract)
uml_declare_architecture_target(uml_memory_contract memory)
target_link_libraries(uml_memory_contract INTERFACE
    UML::GuestPageConfiguration)
target_compile_features(uml_memory_contract INTERFACE cxx_std_20)

set(_uml_static_mmu_sources
    mmu/address_space.cpp
    mmu/execution_gate.cpp
    mmu/mapped_backing.cpp)

add_library(uml_static_mmu OBJECT ${_uml_static_mmu_sources})
uml_declare_architecture_target(uml_static_mmu memory)
target_link_libraries(uml_static_mmu PRIVATE UML::MemoryContract)
target_include_directories(uml_static_mmu PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/mmu)
target_compile_features(uml_static_mmu PUBLIC cxx_std_20)
target_compile_options(uml_static_mmu PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(uml_static_mmu PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=0
    UML_RUNTIME_PROFILING=$<BOOL:${UML_ENABLE_RUNTIME_PROFILING}>)
set_target_properties(uml_static_mmu PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(uml_static_guest_memory OBJECT static_guest_memory.cpp)
uml_declare_architecture_target(uml_static_guest_memory memory)
target_link_libraries(uml_static_guest_memory PRIVATE UML::MemoryContract)
target_include_directories(uml_static_guest_memory PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_static_guest_memory PUBLIC cxx_std_20)
target_compile_options(uml_static_guest_memory PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_static_guest_memory PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_memory_vm_bridge OBJECT mmu/static_api.cpp)
uml_declare_architecture_target(uml_memory_vm_bridge memory)
target_link_libraries(uml_memory_vm_bridge PRIVATE UML::MemoryContract)
target_include_directories(uml_memory_vm_bridge PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin
    ${CMAKE_CURRENT_SOURCE_DIR}/mmu)
target_compile_features(uml_memory_vm_bridge PUBLIC cxx_std_20)
target_compile_options(uml_memory_vm_bridge PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(uml_memory_vm_bridge PRIVATE
    # A caller-supplied native Mach-O kernel also needs this bridge when the
    # embedded ELF runner is disabled. iOS always uses the software MMU.
    UML_MEMORY_HAS_NATIVE_VM=$<BOOL:${UML_TARGET_MACOS}>
    UML_RUNTIME_PROFILING=$<BOOL:${UML_ENABLE_RUNTIME_PROFILING}>)
set_target_properties(uml_memory_vm_bridge PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

if(UML_TARGET_MACOS)
    set(_uml_native_mmu_sources
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu/native_address_space.cpp)
    add_library(uml_native_mmu OBJECT ${_uml_native_mmu_sources})
    uml_declare_architecture_target(uml_native_mmu memory)
    target_link_libraries(uml_native_mmu PRIVATE UML::MemoryContract)
    target_include_directories(uml_native_mmu PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
    target_compile_features(uml_native_mmu PUBLIC cxx_std_20)
    target_compile_options(uml_native_mmu PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(uml_native_mmu PRIVATE
        UML_MEMORY_ALLOCATION_TESTING=0)
    set_target_properties(uml_native_mmu PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Keep complete memory-object composition in the owning module. Consumers
# receive target-owned objects rather than copying a source manifest across
# directory scopes.
add_library(uml_memory_component INTERFACE)
add_library(UML::MemoryComponent ALIAS uml_memory_component)
uml_declare_architecture_target(uml_memory_component memory)
target_link_libraries(uml_memory_component INTERFACE UML::MemoryContract)
target_sources(uml_memory_component INTERFACE
    $<TARGET_OBJECTS:uml_static_mmu>
    $<TARGET_OBJECTS:uml_static_guest_memory>
    $<TARGET_OBJECTS:uml_memory_vm_bridge>)
if(TARGET uml_native_mmu)
    target_sources(uml_memory_component INTERFACE
        $<TARGET_OBJECTS:uml_native_mmu>)
endif()

if(UML_BUILD_UNIT_TESTS)
    # Fault injection and white-box ownership queries recompile the exact
    # production implementation manifests under a test-only policy. Ordinary
    # products contain neither the probe atomics nor their conditional calls.
    add_library(uml_static_mmu_test_variant OBJECT
        ${_uml_static_mmu_sources}
        mmu/allocation_fault.cpp)
    uml_declare_architecture_target(uml_static_mmu_test_variant memory)
    target_link_libraries(uml_static_mmu_test_variant PRIVATE
        UML::MemoryContract)
    target_include_directories(uml_static_mmu_test_variant PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${CMAKE_CURRENT_SOURCE_DIR}/mmu)
    target_compile_features(uml_static_mmu_test_variant PUBLIC cxx_std_20)
    target_compile_options(uml_static_mmu_test_variant PRIVATE
        -Wall -Wextra -Werror)
    target_compile_definitions(uml_static_mmu_test_variant PUBLIC
        UML_MEMORY_ALLOCATION_TESTING=1
        UML_RUNTIME_PROFILING=$<BOOL:${UML_ENABLE_RUNTIME_PROFILING}>)
    set_target_properties(uml_static_mmu_test_variant PROPERTIES
        POSITION_INDEPENDENT_CODE ON)

    if(TARGET uml_native_mmu)
        add_library(uml_native_mmu_test_variant OBJECT
            ${_uml_native_mmu_sources})
        uml_declare_architecture_target(uml_native_mmu_test_variant memory)
        target_link_libraries(uml_native_mmu_test_variant PRIVATE
            UML::MemoryContract)
        target_include_directories(uml_native_mmu_test_variant PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
        target_compile_features(uml_native_mmu_test_variant PUBLIC cxx_std_20)
        target_compile_options(uml_native_mmu_test_variant PRIVATE
            -Wall -Wextra -Werror)
        target_compile_definitions(uml_native_mmu_test_variant PUBLIC
            UML_MEMORY_ALLOCATION_TESTING=1)
        set_target_properties(uml_native_mmu_test_variant PROPERTIES
            POSITION_INDEPENDENT_CODE ON)
    endif()

    add_library(uml_memory_test_component INTERFACE)
    add_library(UML::MemoryTestComponent ALIAS uml_memory_test_component)
    uml_declare_architecture_target(uml_memory_test_component memory)
    target_link_libraries(uml_memory_test_component INTERFACE
        UML::MemoryContract)
    target_compile_definitions(uml_memory_test_component INTERFACE
        UML_MEMORY_ALLOCATION_TESTING=1)
    target_sources(uml_memory_test_component INTERFACE
        $<TARGET_OBJECTS:uml_static_mmu_test_variant>
        $<TARGET_OBJECTS:uml_static_guest_memory>
        $<TARGET_OBJECTS:uml_memory_vm_bridge>)
    if(TARGET uml_native_mmu_test_variant)
        target_sources(uml_memory_test_component INTERFACE
            $<TARGET_OBJECTS:uml_native_mmu_test_variant>)
    endif()
endif()
