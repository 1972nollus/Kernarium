# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_cpu_contract INTERFACE)
add_library(UML::CpuContract ALIAS uml_cpu_contract)
uml_declare_architecture_target(uml_cpu_contract cpu shared)
target_link_libraries(uml_cpu_contract INTERFACE
    UML::MemoryContract
    UML::PlatformContract
    UML::GuestPageConfiguration
    UML::JitProfileConfiguration)
target_compile_features(uml_cpu_contract INTERFACE cxx_std_20)

add_library(uml_arm64_dispatch OBJECT
    arm64/dispatcher.cpp
    arm64/instructions/control_flow.cpp
    arm64/instructions/crc32.cpp
    arm64/instructions/floating_point.cpp
    arm64/instructions/gcs.cpp
    arm64/instructions/integer.cpp
    arm64/instructions/integer/arithmetic.cpp
    arm64/instructions/integer/cpa.cpp
    arm64/instructions/integer/mte.cpp
    arm64/instructions/load_store.cpp
    arm64/instructions/load_store_atomic.cpp
    arm64/instructions/load_store_memory.cpp
    arm64/instructions/simd.cpp
    arm64/instructions/simd/complete.cpp
    arm64/instructions/simd/complete_helpers.cpp
    arm64/instructions/simd/indexed_element.cpp
    arm64/instructions/simd/conversion.cpp
    arm64/instructions/simd/crypto.cpp
    arm64/instructions/simd/floating_compare.cpp
    arm64/instructions/simd/floating_extended.cpp
    arm64/instructions/simd/floating_point.cpp
    arm64/instructions/simd/fp8.cpp
    arm64/instructions/simd/narrowing.cpp
    arm64/instructions/simd/rearrange.cpp
    arm64/instructions/simd/shift.cpp)
uml_declare_architecture_target(uml_arm64_dispatch cpu shared)
target_link_libraries(uml_arm64_dispatch PRIVATE UML::CpuContract)
target_include_directories(uml_arm64_dispatch PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_arm64_dispatch PUBLIC cxx_std_20)
target_compile_options(uml_arm64_dispatch PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_arm64_dispatch PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(uml_arm64_execution_helpers OBJECT
    arm64/code_synchronization.cpp
    arm64/fp_environment.cpp
    arm64/memory_transfer.cpp
    arm64/native_environment.S
    arm64/tlb.cpp)
uml_declare_architecture_target(uml_arm64_execution_helpers cpu shared)
target_link_libraries(uml_arm64_execution_helpers PRIVATE UML::CpuContract)
target_include_directories(uml_arm64_execution_helpers PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_arm64_execution_helpers PUBLIC cxx_std_20)
target_compile_options(uml_arm64_execution_helpers PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>)
set_target_properties(uml_arm64_execution_helpers PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_arm64_gadgets OBJECT
    arm64/gadgets/gadget_memory_selection.cpp
    arm64/gadgets/gadget_scalar_selection.cpp
    arm64/gadgets/gadget_simd_selection.cpp
    arm64/gadgets/program_builder.cpp
    arm64/gadgets/program_builder_branch_system.cpp
    arm64/gadgets/program_builder_data_processing.cpp
    arm64/gadgets/program_builder_exception.cpp
    arm64/gadgets/program_builder_floating_simd.cpp
    arm64/gadgets/program_builder_load_store.cpp
    arm64/gadgets/program_execution.cpp
    arm64/gadgets/software_helpers.cpp
    arm64/gadgets/software_floating_helpers.cpp
    arm64/gadgets/complex_memory_helpers.cpp
    arm64/gadgets/engine.S)
uml_declare_architecture_target(uml_arm64_gadgets cpu gadget)
target_link_libraries(uml_arm64_gadgets PRIVATE UML::CpuContract)
target_link_libraries(uml_arm64_gadgets PUBLIC uml_arm64_execution_helpers)
target_include_directories(uml_arm64_gadgets PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_arm64_gadgets PUBLIC cxx_std_20)
target_compile_options(uml_arm64_gadgets PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>)
set_target_properties(uml_arm64_gadgets PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The decode -> select -> emit chain runs once per translated guest instruction
# and is deliberately spread across thirteen translation units:
# verify_gadget_builder_ownership() requires each decode family to keep an
# independently compiled owner, so neither a unity build nor header inlining is
# available here. Interprocedural optimization is the one route that restores
# the cross-unit inlining that split cost without changing source organization
# or object file names, so that ownership audit still holds.
include(CheckIPOSupported)
check_ipo_supported(RESULT _uml_gadget_ipo_supported
    OUTPUT _uml_gadget_ipo_error LANGUAGES CXX)
if(_uml_gadget_ipo_supported)
    set_target_properties(uml_arm64_gadgets PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION ON)
else()
    message(STATUS
        "gadget translation IPO unavailable: ${_uml_gadget_ipo_error}")
endif()

set(_uml_cpu_jit_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_arena.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_compiler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_compiler_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_compiler_families.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_compiler_crypto.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_compiler_floating.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_compiler_integer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_compiler_memory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_compiler_simd_arithmetic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_compiler_simd_permute.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_emitter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_tlb_translate.S)

add_library(uml_arm64_jit OBJECT ${_uml_cpu_jit_sources})
uml_declare_architecture_target(uml_arm64_jit cpu native)
target_link_libraries(uml_arm64_jit PRIVATE
    UML::CpuContract)
target_include_directories(uml_arm64_jit PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_arm64_jit PUBLIC cxx_std_20)
target_compile_options(uml_arm64_jit PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>)
set_target_properties(uml_arm64_jit PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(_uml_cpu_jit_executor_sources
    arm64/jit/jit_cache.cpp
    arm64/jit/jit_diagnostics.cpp
    arm64/jit/jit_executor_cache.cpp
    arm64/jit/jit_executor_execution.cpp
    arm64/jit/jit_executor_lifecycle.cpp
    arm64/jit/jit_executor_linking.cpp
    arm64/jit/jit_linking.cpp
    arm64/jit/jit_publication_state.cpp
    arm64/jit/jit_semantic_memory.cpp)

add_library(uml_arm64_jit_executor OBJECT
    ${_uml_cpu_jit_executor_sources})
uml_declare_architecture_target(uml_arm64_jit_executor cpu native)
target_link_libraries(uml_arm64_jit_executor PRIVATE UML::CpuContract)
target_link_libraries(uml_arm64_jit_executor PUBLIC
    uml_arm64_jit
    uml_arm64_execution_helpers)
target_include_directories(uml_arm64_jit_executor PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_arm64_jit_executor PUBLIC cxx_std_20)
target_compile_options(uml_arm64_jit_executor PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(uml_arm64_jit_executor PRIVATE
    UML_JIT_PUBLICATION_GATE_TESTING=0
    UML_JIT_TEST_SUPPORT=0
    UML_JIT_RUNTIME_DIAGNOSTICS=0)
set_target_properties(uml_arm64_jit_executor PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

if(UML_BUILD_UNIT_TESTS)
    # Counter-heavy cache/link diagnostics are useful for focused unit tests,
    # but add stores to translation and block-link paths. Recompile only this
    # narrow variant; ordinary products and conformance tests use the exact
    # production executor above with every diagnostic site preprocessed out.
    add_library(uml_arm64_jit_diagnostics_executor OBJECT
        ${_uml_cpu_jit_executor_sources})
    uml_declare_architecture_target(
        uml_arm64_jit_diagnostics_executor cpu native)
    target_link_libraries(uml_arm64_jit_diagnostics_executor PRIVATE
        UML::CpuContract)
    target_link_libraries(uml_arm64_jit_diagnostics_executor PUBLIC
        uml_arm64_jit
        uml_arm64_execution_helpers)
    target_include_directories(uml_arm64_jit_diagnostics_executor PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_features(uml_arm64_jit_diagnostics_executor PUBLIC
        cxx_std_20)
    target_compile_options(uml_arm64_jit_diagnostics_executor PRIVATE
        -Wall -Wextra -Werror)
    target_compile_definitions(uml_arm64_jit_diagnostics_executor PRIVATE
        UML_JIT_PUBLICATION_GATE_TESTING=0
        UML_JIT_TEST_SUPPORT=0
        UML_JIT_RUNTIME_DIAGNOSTICS=1)
    set_target_properties(uml_arm64_jit_diagnostics_executor PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
endif()

# Some CPU implementation translation units contain inline assembly using the
# same optional vocabulary as the standalone assembly sources. Keep that
# option confined to implementation targets that contain those bodies instead
# of widening code generation for every C++ target in an iOS package build.
if(UML_APPLE_CPU_INLINE_ASM_COMPILE_OPTION)
    set(_uml_inline_asm_targets
        uml_arm64_dispatch
        uml_arm64_gadgets
        uml_arm64_jit
        uml_arm64_jit_executor)
    if(TARGET uml_arm64_jit_diagnostics_executor)
        list(APPEND _uml_inline_asm_targets
            uml_arm64_jit_diagnostics_executor)
    endif()
    foreach(_uml_inline_asm_target IN LISTS _uml_inline_asm_targets)
        target_compile_options(${_uml_inline_asm_target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:${UML_APPLE_CPU_INLINE_ASM_COMPILE_OPTION}>)
    endforeach()
    unset(_uml_inline_asm_target)
    unset(_uml_inline_asm_targets)
endif()

if(UML_BUILD_UNIT_TESTS)
    # Test-only diagnostics and publication-gate probes are a separate source
    # owner. Focused tests link this object beside the exact production
    # executor instead of recompiling that executor with test definitions.
    add_library(uml_arm64_jit_test_support OBJECT
        arm64/jit/jit_test_support.cpp)
    uml_declare_architecture_target(uml_arm64_jit_test_support cpu native)
    target_link_libraries(uml_arm64_jit_test_support PRIVATE
        UML::CpuContract)
    target_link_libraries(uml_arm64_jit_test_support PUBLIC
        uml_arm64_jit
        uml_arm64_execution_helpers)
    target_include_directories(uml_arm64_jit_test_support PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_features(uml_arm64_jit_test_support PUBLIC cxx_std_20)
    target_compile_options(uml_arm64_jit_test_support PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>)
    target_compile_definitions(uml_arm64_jit_test_support PRIVATE
        UML_JIT_PUBLICATION_GATE_TESTING=1
        UML_JIT_TEST_SUPPORT=1)
    set_target_properties(uml_arm64_jit_test_support PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
endif()

add_library(uml_arm64_interpreter_engine OBJECT
    arm64/interpreter/engine.cpp)
uml_declare_architecture_target(uml_arm64_interpreter_engine cpu interpreter)
target_link_libraries(uml_arm64_interpreter_engine PRIVATE
    UML::CpuContract)
target_link_libraries(uml_arm64_interpreter_engine PUBLIC uml_arm64_dispatch)
target_include_directories(uml_arm64_interpreter_engine PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_arm64_interpreter_engine PUBLIC cxx_std_20)
target_compile_options(uml_arm64_interpreter_engine PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_arm64_interpreter_engine PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_arm64_gadget_executor OBJECT
    arm64/gadgets/executor.cpp)
uml_declare_architecture_target(uml_arm64_gadget_executor cpu gadget)
target_link_libraries(uml_arm64_gadget_executor PRIVATE
    UML::CpuContract)
target_link_libraries(uml_arm64_gadget_executor PUBLIC uml_arm64_gadgets)
target_include_directories(uml_arm64_gadget_executor PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_arm64_gadget_executor PUBLIC cxx_std_20)
target_compile_options(uml_arm64_gadget_executor PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_arm64_gadget_executor PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_arm64_gadget_engine OBJECT arm64/gadgets/engine_adapter.cpp)
uml_declare_architecture_target(uml_arm64_gadget_engine cpu gadget)
target_link_libraries(uml_arm64_gadget_engine PRIVATE
    UML::CpuContract)
target_link_libraries(uml_arm64_gadget_engine PUBLIC uml_arm64_gadget_executor)
target_include_directories(uml_arm64_gadget_engine PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_arm64_gadget_engine PUBLIC cxx_std_20)
target_compile_options(uml_arm64_gadget_engine PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_arm64_gadget_engine PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_arm64_jit_engine OBJECT arm64/jit/engine_adapter.cpp)
uml_declare_architecture_target(uml_arm64_jit_engine cpu native)
target_link_libraries(uml_arm64_jit_engine PRIVATE
    UML::CpuContract)
target_link_libraries(uml_arm64_jit_engine PUBLIC
    uml_arm64_jit
    uml_arm64_jit_executor)
target_include_directories(uml_arm64_jit_engine PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_arm64_jit_engine PUBLIC cxx_std_20)
target_compile_options(uml_arm64_jit_engine PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_arm64_jit_engine PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_cpu_engine_factory OBJECT engine_factory.cpp)
uml_declare_architecture_target(uml_cpu_engine_factory cpu mixed)
target_link_libraries(uml_cpu_engine_factory PRIVATE
    UML::CpuContract)
target_link_libraries(uml_cpu_engine_factory PUBLIC
    uml_arm64_interpreter_engine
    uml_arm64_gadget_engine
    uml_arm64_jit_engine
    uml_arm64_execution_helpers)
target_include_directories(uml_cpu_engine_factory PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_cpu_engine_factory PUBLIC cxx_std_20)
target_compile_options(uml_cpu_engine_factory PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_cpu_engine_factory PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

# OBJECT libraries keep production and test composition free from duplicate
# compilation, but CMake does not propagate their object files transitively.
# These link components are the supported, self-contained adapter graph for
# consumers that need one engine (or the factory) in isolation.
add_library(uml_cpu_interpreter_component INTERFACE)
uml_declare_architecture_target(uml_cpu_interpreter_component cpu interpreter)
target_link_libraries(uml_cpu_interpreter_component INTERFACE
    UML::CpuContract
    UML::MemoryComponent)
target_sources(uml_cpu_interpreter_component INTERFACE
    $<TARGET_OBJECTS:uml_arm64_interpreter_engine>
    $<TARGET_OBJECTS:uml_arm64_dispatch>)

add_library(uml_cpu_gadget_component INTERFACE)
uml_declare_architecture_target(uml_cpu_gadget_component cpu gadget)
target_link_libraries(uml_cpu_gadget_component INTERFACE
    UML::CpuContract
    UML::MemoryComponent)
target_sources(uml_cpu_gadget_component INTERFACE
    $<TARGET_OBJECTS:uml_arm64_gadget_engine>
    $<TARGET_OBJECTS:uml_arm64_gadget_executor>
    $<TARGET_OBJECTS:uml_arm64_gadgets>
    $<TARGET_OBJECTS:uml_arm64_execution_helpers>
    $<TARGET_OBJECTS:uml_arm64_dispatch>)

add_library(uml_cpu_jit_component INTERFACE)
uml_declare_architecture_target(uml_cpu_jit_component cpu native)
target_link_libraries(uml_cpu_jit_component INTERFACE
    UML::CpuContract
    UML::MemoryComponent)
target_sources(uml_cpu_jit_component INTERFACE
    $<TARGET_OBJECTS:uml_arm64_jit_engine>
    $<TARGET_OBJECTS:uml_arm64_jit>
    $<TARGET_OBJECTS:uml_arm64_jit_executor>
    $<TARGET_OBJECTS:uml_arm64_execution_helpers>
    $<TARGET_OBJECTS:uml_arm64_dispatch>)

# The CPU-owned implementation carrier deliberately excludes memory objects.
# Production and test runtime compositions can pair these exact engine objects
# with the appropriate module-owned memory component without copying either
# module's source manifest.
add_library(uml_cpu_engine_objects INTERFACE)
add_library(UML::CpuEngineObjects ALIAS uml_cpu_engine_objects)
uml_declare_architecture_target(uml_cpu_engine_objects cpu mixed)
target_link_libraries(uml_cpu_engine_objects INTERFACE
    UML::CpuContract)
target_sources(uml_cpu_engine_objects INTERFACE
    $<TARGET_OBJECTS:uml_cpu_engine_factory>
    $<TARGET_OBJECTS:uml_arm64_interpreter_engine>
    $<TARGET_OBJECTS:uml_arm64_dispatch>
    $<TARGET_OBJECTS:uml_arm64_gadget_engine>
    $<TARGET_OBJECTS:uml_arm64_gadget_executor>
    $<TARGET_OBJECTS:uml_arm64_jit_engine>
    $<TARGET_OBJECTS:uml_arm64_jit>
    $<TARGET_OBJECTS:uml_arm64_jit_executor>
    $<TARGET_OBJECTS:uml_arm64_gadgets>
    $<TARGET_OBJECTS:uml_arm64_execution_helpers>)

add_library(uml_cpu_engine_component INTERFACE)
uml_declare_architecture_target(uml_cpu_engine_component cpu mixed)
target_link_libraries(uml_cpu_engine_component INTERFACE
    UML::CpuEngineObjects
    UML::MemoryComponent)
