# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_runtime_contract INTERFACE)
add_library(UML::RuntimeContract ALIAS uml_runtime_contract)
uml_declare_architecture_target(uml_runtime_contract runtime)
target_link_libraries(uml_runtime_contract INTERFACE
    UML::CpuContract
    UML::MemoryContract
    UML::PlatformContract)
target_include_directories(uml_runtime_contract INTERFACE
    ${PROJECT_SOURCE_DIR}/darwin/include)
target_compile_features(uml_runtime_contract INTERFACE cxx_std_20)

add_library(uml_static_runtime OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/static_guest.cpp)
uml_declare_architecture_target(uml_static_runtime runtime)
target_link_libraries(uml_static_runtime PRIVATE
    UML::RuntimeContract
    uml_cpu_engine_factory
    uml_static_guest_memory)
target_include_directories(uml_static_runtime PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_static_runtime PUBLIC cxx_std_20)
target_compile_options(uml_static_runtime PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(uml_static_runtime PRIVATE
    UML_JIT_PUBLICATION_GATE_TESTING=0)
set_target_properties(uml_static_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(uml_runtime_context OBJECT
    ${PROJECT_SOURCE_DIR}/darwin/runtime/context.cpp)
uml_declare_architecture_target(uml_runtime_context runtime)
target_link_libraries(uml_runtime_context PRIVATE
    UML::RuntimeContract
    uml_cpu_engine_factory
    uml_static_guest_memory)
target_include_directories(uml_runtime_context PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_runtime_context PUBLIC cxx_std_20)
target_compile_options(uml_runtime_context PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(uml_runtime_context PRIVATE
    UML_DEFAULT_EXECUTION_ENGINE_JIT=$<BOOL:${UML_TARGET_MACOS}>)
set_target_properties(uml_runtime_context PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_runtime_guest_selector OBJECT
    ${PROJECT_SOURCE_DIR}/darwin/runtime/guest_selector.cpp)
uml_declare_architecture_target(uml_runtime_guest_selector runtime)
target_link_libraries(uml_runtime_guest_selector PRIVATE
    UML::RuntimeContract
    uml_cpu_engine_factory
    uml_static_guest_memory)
target_include_directories(uml_runtime_guest_selector PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_runtime_guest_selector PUBLIC cxx_std_20)
target_compile_options(uml_runtime_guest_selector PRIVATE
    -Wall -Wextra -Werror)
set_target_properties(uml_runtime_guest_selector PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_runtime_entry OBJECT
    ${PROJECT_SOURCE_DIR}/darwin/runtime/bridge.cpp)
uml_declare_architecture_target(uml_runtime_entry runtime)
target_link_libraries(uml_runtime_entry PRIVATE
    UML::RuntimeContract
    uml_cpu_engine_factory
    uml_static_guest_memory)
target_include_directories(uml_runtime_entry PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_runtime_entry PUBLIC cxx_std_20)
target_compile_options(uml_runtime_entry PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_runtime_entry PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

set(UML_RUNTIME_CORE_OBJECTS
	$<TARGET_OBJECTS:uml_runtime_context>
    $<TARGET_OBJECTS:uml_runtime_guest_selector>
    $<TARGET_OBJECTS:uml_runtime_entry>
    $<TARGET_OBJECTS:uml_memory_vm_bridge>
    $<TARGET_OBJECTS:uml_arm64_dispatch>
    $<TARGET_OBJECTS:uml_arm64_gadgets>
    $<TARGET_OBJECTS:uml_static_mmu>
    $<TARGET_OBJECTS:uml_static_runtime>
    $<TARGET_OBJECTS:uml_cpu_engine_factory>
    $<TARGET_OBJECTS:uml_arm64_interpreter_engine>
    $<TARGET_OBJECTS:uml_arm64_gadget_engine>
    $<TARGET_OBJECTS:uml_arm64_jit_engine>
    $<TARGET_OBJECTS:uml_arm64_gadget_executor>
    $<TARGET_OBJECTS:uml_arm64_jit>
    $<TARGET_OBJECTS:uml_arm64_jit_executor>
    $<TARGET_OBJECTS:uml_static_guest_memory>)
if(TARGET uml_native_mmu)
    list(APPEND UML_RUNTIME_CORE_OBJECTS
        $<TARGET_OBJECTS:uml_native_mmu>)
endif()
set(UML_RUNTIME_CORE_OBJECTS "${UML_RUNTIME_CORE_OBJECTS}" PARENT_SCOPE)

# Keep a separately inspectable runtime archive while exposing the exact same
# owned object composition to the complete Apple library product.
add_library(uml_runtime_core STATIC ${UML_RUNTIME_CORE_OBJECTS})
add_library(UML::RuntimeCore ALIAS uml_runtime_core)
uml_declare_architecture_target(uml_runtime_core runtime)
target_link_libraries(uml_runtime_core PRIVATE UML::RuntimeContract)
target_include_directories(uml_runtime_core PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_runtime_core PUBLIC cxx_std_20)
target_compile_options(uml_runtime_core PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_runtime_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME uml_runtime_core)
