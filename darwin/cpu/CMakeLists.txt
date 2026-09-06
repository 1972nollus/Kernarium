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

# Off by default so ordinary translation has no diagnostic instrumentation.
# Build a separate tree with -DUML_GADGET_PAIR_HISTOGRAM=ON and set
# DARWIN_UML_GADGET_PAIR_HISTOGRAM to a path to capture which adjacent gadget
# pairs a workload actually translates, which is what G-2's remaining step needs
# in order to choose fusion candidates by frequency rather than by eye.
option(UML_GADGET_PAIR_HISTOGRAM
    "Count adjacent gadget pairs during translation (diagnostic builds only)"
    OFF)

option(UML_GADGET_DEPENDENCY_COVERAGE
    "Count executed scalar and SIMD/FP dependency opportunities (diagnostic builds only)"
    OFF)

# Also default-off and mutually exclusive with dependency coverage: the same
# threaded counter gadget indexes a table keyed by the exact selected entry,
# which is what a static host-instruction cost per entry must be weighted by.
option(UML_GADGET_EXECUTION_HISTOGRAM
    "Count executed gadget entries (diagnostic builds only)"
    OFF)

option(UML_GADGET_SCALAR_FORWARDING
    "Forward adjacent write-through scalar results within gadget blocks"
    ON)

option(UML_GADGET_VECTOR_FORWARDING
    "Forward adjacent write-through SIMD/FP results within gadget blocks"
    ON)

# Also off by default, and for the same reason. G-4 needs to know how far a
# fixed guest-to-host vector bank would reach, and that is decided by how
# concentrated translated code's vector operand registers are.
option(UML_GADGET_SIMD_OPERAND_HISTOGRAM
    "Count guest SIMD operand registers during translation (diagnostic only)"
    OFF)

# G107's bounded successor fetch applies only to the 32 non-faulting
# ADD/SUB-immediate entries whose register and stream contract can safely load
# the next gadget together with their operand. Focused and full Geekbench
# measurements support enabling that bounded family in ordinary products.
option(UML_GADGET_ADD_SUB_IMMEDIATE_SUCCESSOR_HOIST
    "Hoist successor fetch in ADD/SUB-immediate gadgets"
    ON)

# Keep the accepted destination-residency family separately selectable for
# focused regression tests. The implementation never carries mutable vector
# state across a translated block.
option(UML_GADGET_SIMD_DESTINATION_RESIDENCY
    "Keep one bounded SIMD MLA destination resident within a block"
    ON)

# Retain decoded programs across a code-change sequence only after validating
# every guest word the program consumed. Direct and indirect activations are
# reset before any retained program can be reached again.
option(UML_GADGET_EXACT_CACHE_VALIDATION
    "Reuse exactly validated gadget programs after guest code changes"
    ON)

# Slots are tagged with the address space they describe, so one pool serves all
# of them and a switch retains rather than discards. Capacity is therefore a
# single shared budget: a busy address space may use more than an even share,
# and an idle one costs nothing.
# 65536 slots with eight resident address spaces: measured on a browser guest,
# that reaches a 76.2% lookup hit rate against 29.7% for flushing on every
# switch, and cuts retranslation from 1.46M to 255K a second. Sixteen resident
# was measured and is worse on every axis -- more address spaces dilute one
# pool the same way partitioning it did.
set(UML_GADGET_CACHE_SLOTS "65536" CACHE STRING
    "Slots in the gadget cache; power of two, at least 1024")
set(UML_GADGET_RESIDENT_GENERATIONS "8" CACHE STRING
    "Address spaces that may hold translations at once")
# A block otherwise ends at its first branch, which on a browser guest is a mean
# of 4.93 instructions -- too short for anything per-block to amortize against.
# Conditional branches end 52.8% of blocks and are the only large category whose
# fall-through is the next address, so continuing through them leaves fault
# accounting and the G140/G141 validation contract untouched. Worth 60.1% of
# frame rate on WebGL Aquarium; see G162.
option(UML_GADGET_INLINE_CONDITIONAL_FALLTHROUGH
    "Translate a conditional branch's fall-through inline behind it" ON)
# Experimental A/B arm, off by default. Once conditional fall-throughs are
# inlined, 48.9% of blocks end at a direct call, and inlining the callee removes
# both that boundary and the indirect return. Measured on the corrected code it
# is +1.7% at p = 0.43 over eight runs an arm -- not demonstrated -- and
# inlining at every site rather than once per callee is an exact wash, its
# 3.43x duplication cancelling what the removed boundaries buy. See G163.
option(UML_GADGET_INLINE_CALLS
    "Translate a direct call's callee inline behind it" OFF)
# The fetch window, not a limit on block length: only 0.07% of blocks run far
# enough to be cut off by it rather than ending at a branch.
set(UML_GADGET_BLOCK_INSTRUCTIONS "64" CACHE STRING
    "Guest instructions fetched per translation attempt")
# Experimental A/B arm, off by default. Every vCPU carries its own executor and
# its own cache, so a guest running on several of them translates the same code
# once per vCPU -- 69% of translations at four vCPUs and 80% at eight were work
# another executor had already done. Letting them adopt each other's
# translations removes most of that work.
#
# At 128 MiB and the block sizes that predated G162 it cost frame rate, because
# an adoption was two to three times the cost of the build it replaced (G161).
# Remeasured at 512 MiB on the current, longer blocks it is instead an exact
# swap: a shared tier serves 56% of private misses, Builder::build falls from
# 4.66% of vCPU on-CPU time to 2.32% and the store itself takes 2.31%, leaving
# frame rate at -0.89% (t = -0.49, n = 4 an arm) for 512 MiB of host memory.
# Building is only a few percent of the time available to save, so the tier
# cannot pay for itself however cheap adoption becomes. Paying for it by
# shrinking the private cache to 16384 slots is far worse: the private hit rate
# collapses to 72.7%, 96.8% of stores evict a live block, and frame rate falls
# 26.3%. See G166; do not enable without re-measuring.
set(UML_GADGET_SHARED_BUDGET_MIB "0" CACHE STRING
    "Process-wide budget for shared gadget translations, in MiB; 0 disables")
math(EXPR _uml_gadget_shared_budget_bytes
    "${UML_GADGET_SHARED_BUDGET_MIB} * 1024 * 1024")

add_library(uml_arm64_gadgets OBJECT
    arm64/gadgets/gadget_memory_selection.cpp
    arm64/gadgets/gadget_scalar_selection.cpp
    arm64/gadgets/gadget_simd_selection.cpp
    arm64/gadgets/pair_histogram.cpp
    arm64/gadgets/dependency_coverage.cpp
    arm64/gadgets/execution_histogram.cpp
    arm64/gadgets/scalar_forwarding.cpp
    arm64/gadgets/simd_operand_histogram.cpp
    # program_builder.cpp textually composes the five decode families from
    # their *_impl.ipp fragments so the whole per-instruction decode chain
    # inlines as one unit; see the comment at the top of that file.
    arm64/gadgets/program_builder.cpp
    arm64/gadgets/program_execution.cpp
    arm64/gadgets/shared_programs.cpp
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
target_compile_definitions(uml_arm64_gadgets PRIVATE
    UML_GADGET_PAIR_HISTOGRAM=$<BOOL:${UML_GADGET_PAIR_HISTOGRAM}>
    UML_GADGET_DEPENDENCY_COVERAGE=$<BOOL:${UML_GADGET_DEPENDENCY_COVERAGE}>
    UML_GADGET_EXECUTION_HISTOGRAM=$<BOOL:${UML_GADGET_EXECUTION_HISTOGRAM}>
    UML_GADGET_SCALAR_FORWARDING=$<BOOL:${UML_GADGET_SCALAR_FORWARDING}>
    UML_GADGET_VECTOR_FORWARDING=$<BOOL:${UML_GADGET_VECTOR_FORWARDING}>
    UML_GADGET_SIMD_OPERAND_HISTOGRAM=$<BOOL:${UML_GADGET_SIMD_OPERAND_HISTOGRAM}>
    UML_GADGET_ADD_SUB_IMMEDIATE_SUCCESSOR_HOIST=$<BOOL:${UML_GADGET_ADD_SUB_IMMEDIATE_SUCCESSOR_HOIST}>
    UML_GADGET_SIMD_DESTINATION_RESIDENCY=$<BOOL:${UML_GADGET_SIMD_DESTINATION_RESIDENCY}>
    UML_GADGET_EXACT_CACHE_VALIDATION=$<BOOL:${UML_GADGET_EXACT_CACHE_VALIDATION}>
    UML_GADGET_INLINE_CONDITIONAL_FALLTHROUGH=$<BOOL:${UML_GADGET_INLINE_CONDITIONAL_FALLTHROUGH}>
    UML_GADGET_INLINE_CALLS=$<BOOL:${UML_GADGET_INLINE_CALLS}>
    UML_GADGET_SHARED_BUDGET_BYTES=${_uml_gadget_shared_budget_bytes}UL)

# The gadget library is still many translation units, and the selection tables
# the decode families call into are among them. Interprocedural optimization
# restores the cross-unit inlining that splitting the library cost; it is worth
# 4.2% multi-core on its own. It is not sufficient by itself - ThinLTO imports
# on a cost model that declines functions as large as the decode families, which
# is why those five are composed textually into program_builder.cpp instead.
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
    arm64/jit/jit_cache_geometry.cpp
    arm64/jit/jit_diagnostics.cpp
    arm64/jit/jit_executor_cache.cpp
    arm64/jit/jit_executor_execution.cpp
    arm64/jit/jit_executor_lifecycle.cpp
    arm64/jit/jit_executor_pool.cpp
    arm64/jit/jit_executor_linking.cpp
    arm64/jit/jit_linking.cpp
    arm64/jit/jit_publication_state.cpp
    arm64/jit/jit_semantic_memory.cpp
    arm64/jit/jit_shared_translations.cpp)
if(UML_ENABLE_JIT_PROFILE)
    list(APPEND _uml_cpu_jit_executor_sources
        arm64/jit/jit_executor_pool_profile.cpp
        arm64/jit/jit_profile_api.cpp
        arm64/jit/jit_profile_output.cpp)
endif()

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
target_compile_definitions(uml_arm64_gadget_executor PRIVATE
    UML_GADGET_EXACT_CACHE_VALIDATION=$<BOOL:${UML_GADGET_EXACT_CACHE_VALIDATION}>
    UML_GADGET_EXECUTION_HISTOGRAM=$<BOOL:${UML_GADGET_EXECUTION_HISTOGRAM}>
    UML_GADGET_CACHE_SLOTS=${UML_GADGET_CACHE_SLOTS}
    UML_GADGET_BLOCK_INSTRUCTIONS=${UML_GADGET_BLOCK_INSTRUCTIONS}
    UML_GADGET_INLINE_CALLS=$<BOOL:${UML_GADGET_INLINE_CALLS}>
    UML_GADGET_RESIDENT_GENERATIONS=${UML_GADGET_RESIDENT_GENERATIONS})
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

# OBJECT libraries avoid duplicate compilation, but CMake does not propagate
# their contents transitively. These link components expose the supported
# adapter graph for consumers that select one engine or the factory.
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
