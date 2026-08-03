# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_memory_contract INTERFACE)
add_library(UML::MemoryContract ALIAS uml_memory_contract)
uml_declare_architecture_target(uml_memory_contract memory)
target_link_libraries(uml_memory_contract INTERFACE
    UML::GuestPageConfiguration)
target_compile_features(uml_memory_contract INTERFACE cxx_std_20)

add_library(uml_static_mmu OBJECT
    mmu/address_space.cpp
    mmu/execution_gate.cpp
    mmu/mapped_backing.cpp)
uml_declare_architecture_target(uml_static_mmu memory)
target_link_libraries(uml_static_mmu PRIVATE UML::MemoryContract)
target_include_directories(uml_static_mmu PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/mmu)
target_compile_features(uml_static_mmu PUBLIC cxx_std_20)
target_compile_options(uml_static_mmu PRIVATE -Wall -Wextra -Werror)
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
    UML_MEMORY_HAS_NATIVE_VM=$<BOOL:${UML_TARGET_MACOS}>)
set_target_properties(uml_memory_vm_bridge PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

if(NOT UML_GUEST_16K_PAGES)
    add_library(uml_native_mmu OBJECT
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu/native_address_space.cpp)
    uml_declare_architecture_target(uml_native_mmu memory)
    target_link_libraries(uml_native_mmu PRIVATE UML::MemoryContract)
    target_include_directories(uml_native_mmu PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
    target_compile_features(uml_native_mmu PUBLIC cxx_std_20)
    target_compile_options(uml_native_mmu PRIVATE -Wall -Wextra -Werror)
    set_target_properties(uml_native_mmu PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
