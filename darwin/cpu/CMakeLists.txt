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

# A private cache holds one translation per slot, so the slot count is the
# working set an executor can keep. Counted on Geekbench 6 Clang, the workload
# with the largest code footprint, one executor reaches about 194,000 distinct
# x86 blocks and 134,000 arm64 blocks, against 65,536 slots: the cache runs at
# 100% occupancy, 89% of translations are of a block that executor had already
# translated and lost, and address-space retirement causes none of them. The
# slot count, not residency or validation, is what bounds Clang.
#
# Capacity is not free on either engine. An empty way costs 56 bytes on arm64
# and 76 on x86, and an occupied one its translation besides. macOS can spend
# it and iOS cannot: the iOS app runs under a jetsam limit with a 2048 MiB
# guest default, and one executor per host core would pay for it several times
# over. The compatibility default therefore stays where it was measured to be
# affordable.
if(UML_APPLE_PLATFORM STREQUAL "macos")
    set(_uml_gadget_cache_slots_default "262144")
else()
    set(_uml_gadget_cache_slots_default "65536")
endif()
set(UML_GADGET_CACHE_SLOTS "${_uml_gadget_cache_slots_default}" CACHE STRING
    "Slots in each ISA's private gadget cache; power of two, at least 1024")
unset(_uml_gadget_cache_slots_default)
if(NOT UML_GADGET_CACHE_SLOTS MATCHES "^[0-9]+$")
    message(FATAL_ERROR
        "UML_GADGET_CACHE_SLOTS must be a positive integer")
endif()
math(EXPR _uml_gadget_cache_slots_invalid
    "${UML_GADGET_CACHE_SLOTS} & (${UML_GADGET_CACHE_SLOTS} - 1)")
if(UML_GADGET_CACHE_SLOTS LESS 1024 OR
   NOT _uml_gadget_cache_slots_invalid EQUAL 0)
    message(FATAL_ERROR
        "UML_GADGET_CACHE_SLOTS must be a power of two of at least 1024, "
        "not ${UML_GADGET_CACHE_SLOTS}")
endif()
unset(_uml_gadget_cache_slots_invalid)
set(UML_GADGET_RESIDENT_GENERATIONS "8" CACHE STRING
    "Address spaces that may hold translations in each private gadget cache")

# Default-off: the census walks every cache slot when an executor is destroyed
# and writes its measured footprint plus one record per resident translation,
# which is how duplication between executors and between address spaces is
# measured rather than estimated. Both engines report it, so the option is
# declared with the shared cache geometry above rather than beside the arm64
# translation diagnostics below.
option(UML_GADGET_CACHE_CENSUS
    "Dump resident gadget cache contents at executor teardown (diagnostic builds only)"
    OFF)

# x86-64 code is translated to immutable streams of precompiled host gadgets.
# The fixed x86 Linux slice uses a 4 KiB guest profile on macOS, whose
# dependency prefix provides the exact math archives.
if(UML_BUILD_X86_64_GUEST)
    add_library(uml_x86_64_gadgets OBJECT
        x86_64/cpu_state.cpp
        x86_64/decoder.cpp
		x86_64/gadgets/code_page_registry.cpp
		x86_64/gadgets/engine.S
		x86_64/gadgets/executor.cpp
		x86_64/gadgets/native_builder.cpp
		x86_64/gadgets/native_memory.cpp
        x86_64/gadgets/program.cpp
		x86_64/gadgets/x87_math.cpp)
    uml_declare_architecture_target(uml_x86_64_gadgets cpu gadget)
    target_link_libraries(uml_x86_64_gadgets
        PUBLIC UML::ThirdPartyCephes128
        PRIVATE UML::CpuContract)
    target_include_directories(uml_x86_64_gadgets PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_features(uml_x86_64_gadgets PUBLIC cxx_std_20)
    target_compile_options(uml_x86_64_gadgets PRIVATE
        -Wall -Wextra -Werror)
    target_compile_definitions(uml_x86_64_gadgets PRIVATE
        UML_GADGET_CACHE_CENSUS=$<BOOL:${UML_GADGET_CACHE_CENSUS}>
        UML_GADGET_CACHE_SLOTS=${UML_GADGET_CACHE_SLOTS}
        UML_GADGET_RESIDENT_GENERATIONS=${UML_GADGET_RESIDENT_GENERATIONS})
    set_target_properties(uml_x86_64_gadgets PROPERTIES
        POSITION_INDEPENDENT_CODE ON)

    # The register-baked scalar bank is enumerated by a generator: one entry
    # per operand combination is thousands of entries, which the assembler's
    # own macro expansion takes minutes to produce.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_uml_x86_64_baked_dir ${CMAKE_CURRENT_BINARY_DIR}/x86_64_baked)
    set(_uml_x86_64_baked_generator
        ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/gadgets/engine/generate_baked.py)
    add_custom_command(
        OUTPUT ${_uml_x86_64_baked_dir}/baked_scalar.inc
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_uml_x86_64_baked_dir}
        COMMAND ${Python3_EXECUTABLE} -B ${_uml_x86_64_baked_generator}
            --output ${_uml_x86_64_baked_dir}/baked_scalar.inc
        DEPENDS ${_uml_x86_64_baked_generator}
        COMMENT "Generating the x86-64 register-baked gadget bank"
        VERBATIM)
    set_source_files_properties(x86_64/gadgets/engine.S PROPERTIES
        OBJECT_DEPENDS ${_uml_x86_64_baked_dir}/baked_scalar.inc)
    target_include_directories(uml_x86_64_gadgets PRIVATE
        ${_uml_x86_64_baked_dir})
endif()

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

# Off by default so ordinary translation has no diagnostic instrumentation. A
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

# An in-place "add Rd, Rd, #imm" writes and re-reads one CpuState slot;
# addressing that slot with a baked displacement instead of a decoded register
# index lets the host forward the store to the load at rename. Bounded at 31
# entries per operation because the in-place shape needs one index for both
# accesses. Turning this on without UML_GADGET_INPLACE_MEMORY leaves a baked
# producer feeding an indexed consumer, which is slower than baking neither, so
# the two belong on or off together.
option(UML_GADGET_INPLACE_IMMEDIATE
    "Bake the CpuState displacement into in-place ADD/SUB-immediate entries"
    ON)

# The companion to the entry above. An unsigned-offset transfer reaches CpuState
# through the register it moves; baking that displacement both forwards the
# value at rename and keeps the two ends of the hop in the same addressing form,
# which a baked arithmetic entry alone does not. Covers the 32- and 64-bit forms
# at 31 entries per mode, whose cold tails are out of line so the copies cost
# only what differs between them.
option(UML_GADGET_INPLACE_MEMORY
    "Bake the CpuState displacement into unsigned-offset scalar transfer entries"
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
# A block otherwise ends at its first branch, which on a browser guest is a mean
# of 4.93 instructions -- too short for anything per-block to amortize against.
# Conditional branches end 52.8% of blocks and are the only large category whose
# fall-through is the next address, so continuing through them leaves fault
# accounting and the G140/G141 validation contract untouched. Worth 60.1% of
# frame rate on WebGL Aquarium; see G162.
option(UML_GADGET_INLINE_CONDITIONAL_FALLTHROUGH
    "Translate a conditional branch's fall-through inline behind it" ON)
# Same-page transfers fold the cross-page check into the TLB tag compare
# (16 KiB pages), fall through on a hit, and keep the second way, crossing and
# miss handling behind the dispatch. OFF is the matched-layout control for A/B
# timing; it pads each wider scalar transfer by one word so both builds keep
# every later entry at the same address.
option(UML_GADGET_FUSED_TRANSLATE
    "Fold the transfer cross-page check into the TLB compare with a fall-through hit path" ON)
# Linked calls push the return block's link on a private sixteen-record ring
# and a cached return whose site cells miss pops a matching record, training
# the cell. Exact counters over the Geekbench 6 single-core set: executor round
# trips -59 percent, return misses -84 percent; HDR +9, Clang +4, HTML5 +4.5
# percent on a quiet host with the aggregate inside the shared-host band.
option(UML_GADGET_RETURN_STACK
    "Predict cached returns through a private return-record ring" ON)
# After a site's cells (and for a return the ring) miss, look the target up in
# a direct-mapped {target, BlockLink} table the address space's misses fill,
# instead of returning to the executor. Most Geekbench 6 single-core round
# trips are misses at sites with more targets than any cell array covers.
option(UML_GADGET_TARGET_TIER
    "Follow indirect-site misses through a per-address-space target table" ON)
# The fetch window, not a limit on block length: only 0.07% of blocks run far
# enough to be cut off by it rather than ending at a branch.
set(UML_GADGET_BLOCK_INSTRUCTIONS "64" CACHE STRING
    "Guest instructions fetched per translation attempt")
add_library(uml_arm64_gadgets OBJECT
    arm64/gadgets/gadget_memory_selection.cpp
    arm64/gadgets/gadget_scalar_selection.cpp
    arm64/gadgets/gadget_simd_selection.cpp
    arm64/gadgets/execution_histogram.cpp
    arm64/gadgets/guest_sampler.cpp
    arm64/gadgets/scalar_forwarding.cpp
    # program_builder.cpp textually composes the five decode families from
    # their *_impl.ipp fragments so the whole per-instruction decode chain
    # inlines as one unit; see the comment at the top of that file.
    arm64/gadgets/program_builder.cpp
    arm64/gadgets/program_execution.cpp
    arm64/gadgets/software_helpers.cpp
    arm64/gadgets/software_floating_helpers.cpp
    arm64/gadgets/software_crypto_helpers.cpp
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
    UML_GADGET_EXECUTION_HISTOGRAM=$<BOOL:${UML_GADGET_EXECUTION_HISTOGRAM}>
    UML_GADGET_SCALAR_FORWARDING=$<BOOL:${UML_GADGET_SCALAR_FORWARDING}>
    UML_GADGET_VECTOR_FORWARDING=$<BOOL:${UML_GADGET_VECTOR_FORWARDING}>
    UML_GADGET_ADD_SUB_IMMEDIATE_SUCCESSOR_HOIST=$<BOOL:${UML_GADGET_ADD_SUB_IMMEDIATE_SUCCESSOR_HOIST}>
    UML_GADGET_INPLACE_IMMEDIATE=$<BOOL:${UML_GADGET_INPLACE_IMMEDIATE}>
    UML_GADGET_INPLACE_MEMORY=$<BOOL:${UML_GADGET_INPLACE_MEMORY}>
    UML_GADGET_SIMD_DESTINATION_RESIDENCY=$<BOOL:${UML_GADGET_SIMD_DESTINATION_RESIDENCY}>
    UML_GADGET_EXACT_CACHE_VALIDATION=$<BOOL:${UML_GADGET_EXACT_CACHE_VALIDATION}>
    UML_GADGET_INLINE_CONDITIONAL_FALLTHROUGH=$<BOOL:${UML_GADGET_INLINE_CONDITIONAL_FALLTHROUGH}>
    UML_GADGET_FUSED_TRANSLATE=$<BOOL:${UML_GADGET_FUSED_TRANSLATE}>
    UML_GADGET_RETURN_STACK=$<BOOL:${UML_GADGET_RETURN_STACK}>
    UML_GADGET_TARGET_TIER=$<BOOL:${UML_GADGET_TARGET_TIER}>)

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
    # The x86-64 backend splits the same way: its decoder, builder, program
    # cache and TLB miss handler are separate units on the translation path.
    if(TARGET uml_x86_64_gadgets)
        set_target_properties(uml_x86_64_gadgets PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
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
    arm64/jit/jit_cache_geometry.cpp
    arm64/jit/jit_diagnostics.cpp
    arm64/jit/jit_executor_cache.cpp
    arm64/jit/jit_executor_execution.cpp
    arm64/jit/jit_executor_lifecycle.cpp
    arm64/jit/jit_executor_pool.cpp
    arm64/jit/jit_executor_linking.cpp
    arm64/jit/jit_linking.cpp
    arm64/jit/jit_publication_state.cpp
    arm64/jit/jit_semantic_atomic.cpp
    arm64/jit/jit_semantic_classes.cpp
    arm64/jit/jit_semantic_control.cpp
    arm64/jit/jit_semantic_load_store.cpp
    arm64/jit/jit_semantic_memory.cpp
    arm64/jit/jit_semantic_reservation.cpp
    arm64/jit/jit_shared_translations.cpp)
if(UML_ENABLE_RUNTIME_PROFILING)
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
    UML_GADGET_CACHE_CENSUS=$<BOOL:${UML_GADGET_CACHE_CENSUS}>
    UML_GADGET_CACHE_SLOTS=${UML_GADGET_CACHE_SLOTS}
    UML_GADGET_BLOCK_INSTRUCTIONS=${UML_GADGET_BLOCK_INSTRUCTIONS}
    UML_GADGET_TARGET_TIER=$<BOOL:${UML_GADGET_TARGET_TIER}>
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
add_library(uml_cpu_gadget_component INTERFACE)
uml_declare_architecture_target(uml_cpu_gadget_component cpu gadget)
target_link_libraries(uml_cpu_gadget_component INTERFACE
    UML::CpuContract
    UML::MemoryComponent)
target_sources(uml_cpu_gadget_component INTERFACE
    $<TARGET_OBJECTS:uml_arm64_gadget_engine>
    $<TARGET_OBJECTS:uml_arm64_gadget_executor>
    $<TARGET_OBJECTS:uml_arm64_gadgets>
    $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
if(TARGET uml_x86_64_gadgets)
    target_link_libraries(uml_cpu_gadget_component INTERFACE
        UML::ThirdPartyCephes128)
    target_sources(uml_cpu_gadget_component INTERFACE
        $<TARGET_OBJECTS:uml_x86_64_gadgets>)
endif()

add_library(uml_cpu_jit_component INTERFACE)
uml_declare_architecture_target(uml_cpu_jit_component cpu native)
target_link_libraries(uml_cpu_jit_component INTERFACE
    UML::CpuContract
    UML::MemoryComponent)
target_sources(uml_cpu_jit_component INTERFACE
    $<TARGET_OBJECTS:uml_arm64_jit_engine>
    $<TARGET_OBJECTS:uml_arm64_jit>
    $<TARGET_OBJECTS:uml_arm64_jit_executor>
    $<TARGET_OBJECTS:uml_arm64_execution_helpers>)

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
    $<TARGET_OBJECTS:uml_arm64_gadget_engine>
    $<TARGET_OBJECTS:uml_arm64_gadget_executor>
    $<TARGET_OBJECTS:uml_arm64_jit_engine>
    $<TARGET_OBJECTS:uml_arm64_jit>
    $<TARGET_OBJECTS:uml_arm64_jit_executor>
    $<TARGET_OBJECTS:uml_arm64_gadgets>
    $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
if(TARGET uml_x86_64_gadgets)
    target_link_libraries(uml_cpu_engine_objects INTERFACE
        UML::ThirdPartyCephes128)
    target_sources(uml_cpu_engine_objects INTERFACE
        $<TARGET_OBJECTS:uml_x86_64_gadgets>)
endif()

add_library(uml_cpu_engine_component INTERFACE)
uml_declare_architecture_target(uml_cpu_engine_component cpu mixed)
target_link_libraries(uml_cpu_engine_component INTERFACE
    UML::CpuEngineObjects
    UML::MemoryComponent)
