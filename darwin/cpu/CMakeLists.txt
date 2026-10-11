# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_cpu_contract INTERFACE)
add_library(UML::CpuContract ALIAS uml_cpu_contract)
uml_declare_architecture_target(uml_cpu_contract cpu shared)
target_link_libraries(uml_cpu_contract INTERFACE
    UML::MemoryContract
    UML::PlatformContract
    UML::GuestPageConfiguration)

# Executable memory has one owner, which the JIT engines share.
add_subdirectory(jit_memory)

# The AArch32 decoder both arm64 engines and the semantic library share.
add_subdirectory(arm64/aarch32)

# A private cache holds one translation per slot, so the slot count is the
# working set an executor can keep. Counted on Geekbench 6 Clang, the workload
# with the largest code footprint, one executor reaches about 194,000 distinct
# x86 blocks and 134,000 arm64 blocks, against 65,536 slots: the cache runs at
# 100% occupancy, 89% of translations are of a block that executor had already
# translated and lost, and address-space retirement causes none of them. The
# slot count, not residency or validation, is what bounds Clang.
#
# Capacity is not free on either engine. An empty way costs 40 bytes on arm64
# and 80 on x86, and an occupied one its translation besides. macOS can spend
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
# Slots are tagged with the address space they describe, so one pool serves all
# of them and a switch retains rather than discards. Eight resident address
# spaces, measured on a browser guest at 65536 slots, reach a 76.2% lookup hit
# rate against 29.7% for flushing on every switch and cut retranslation from
# 1.46M to 255K a second; sixteen was worse on every axis, because more address
# spaces dilute one pool the same way partitioning it did. At macOS's 262,144
# slots more address spaces do hit more often, but each resident one keeps its
# link records until it retires: twelve and sixteen grew the browser's cache by
# two fifths and two thirds, and with half the slots to pay for that they hit
# less than eight. Eight stays the default on both products (G227).
set(UML_GADGET_RESIDENT_GENERATIONS "8" CACHE STRING
    "Address spaces that may hold translations in each private gadget cache")

# Default-off: the census walks every cache slot when an executor is destroyed,
# and at intervals when asked, and writes its measured footprint, its lookup
# and eviction history, and one record per resident translation, which is how
# duplication between executors and between address spaces is measured rather
# than estimated. Both engines report it, so the option is declared with the
# shared cache geometry above rather than beside the arm64 translation
# diagnostics below.
option(UML_GADGET_CACHE_CENSUS
    "Write gadget cache census snapshots (diagnostic builds only)"
    OFF)

# x86-64 guests run on the x86_64 tree's engines. The fixed x86 Linux slice
# uses a 4 KiB guest profile on macOS.
if(UML_BUILD_X86_64_GUEST)
    add_subdirectory(x86_64)
endif()

add_library(uml_arm64_execution_helpers OBJECT
    arm64/code_synchronization.cpp
    arm64/floating_exception.cpp
    arm64/fp_environment.cpp
    arm64/memory_transfer.cpp
    arm64/native_environment.S
    arm64/tlb.cpp)
uml_declare_architecture_target(uml_arm64_execution_helpers cpu shared)
target_link_libraries(uml_arm64_execution_helpers PRIVATE
    UML::BridgeAbi
    UML::CpuContract)
target_include_directories(uml_arm64_execution_helpers PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)

# The engine-neutral SVE semantic library (docs/architecture.md). The gadget
# engine calls it (arm64/gadgets/sve_helpers.cpp), so every composition that
# carries the gadget objects carries its objects beside them.
add_library(uml_arm64_sve_semantics OBJECT
    arm64/detail/sve_semantics.cpp
    arm64/detail/sve_semantics_crypto.cpp
    arm64/detail/sve_semantics_floating.cpp
    arm64/detail/sve_semantics_integer.cpp
    arm64/detail/sve_semantics_memory.cpp
    arm64/detail/sve_semantics_permute.cpp
    arm64/detail/sve_semantics_predicate.cpp
    arm64/detail/sve_semantics_wide.cpp)
uml_declare_architecture_target(uml_arm64_sve_semantics cpu shared)
target_link_libraries(uml_arm64_sve_semantics PRIVATE
    UML::BridgeAbi
    UML::CpuContract)
target_include_directories(uml_arm64_sve_semantics PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)

# Off by default so ordinary translation has no diagnostic instrumentation. A
# threaded counter gadget indexes a table keyed by the exact selected entry,
# which is what a static host-instruction cost per entry must be weighted by.
option(UML_GADGET_EXECUTION_HISTOGRAM
    "Count executed gadget entries (diagnostic builds only)"
    OFF)
# GadgetExecutor's layout follows this switch, so every consumer of the gadget
# headers must see the value the library was built with, and
# execution_histogram.hpp refuses to compile without one. The CPU contract
# carries it to CPU consumers, and the unit suite links it into every test
# binary, including those that compose the gadget objects directly.
add_library(uml_gadget_histogram_configuration INTERFACE)
add_library(UML::GadgetHistogramConfiguration ALIAS
    uml_gadget_histogram_configuration)
uml_declare_architecture_target(uml_gadget_histogram_configuration cpu shared)
target_compile_definitions(uml_gadget_histogram_configuration INTERFACE
    UML_GADGET_EXECUTION_HISTOGRAM=$<BOOL:${UML_GADGET_EXECUTION_HISTOGRAM}>)
target_link_libraries(uml_cpu_contract INTERFACE
    UML::GadgetHistogramConfiguration)

# Gadget-engine SME sources, owned by SME lane S5: the SME part's selection
# and helper sources. The streaming gadget bodies it generates at build time
# from the shared SVE/SME encoding table (cpu/arm64/detail/sme_encodings.hpp)
# are declared in this block too, with their custom command and the
# OBJECT_DEPENDS of engine.S. The JIT's block below is disjoint, so the two
# lanes never edit the same lines.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_uml_arm64_streaming_dir ${CMAKE_CURRENT_BINARY_DIR}/arm64_streaming)
set(_uml_arm64_streaming_generator
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/gadgets/engine/generate_streaming.py)
set(_uml_arm64_streaming_loader
    ${PROJECT_SOURCE_DIR}/tools/arm64-tests/sme_encoding_table.py)
set(_uml_arm64_streaming_table
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/detail/sme_encodings.hpp)
set(_uml_arm64_streaming_assembly
    ${_uml_arm64_streaming_dir}/streaming_generated.inc
    ${_uml_arm64_streaming_dir}/streaming_memory.inc)
add_custom_command(
    OUTPUT ${_uml_arm64_streaming_assembly}
        ${_uml_arm64_streaming_dir}/streaming_forms.hpp
    COMMAND ${Python3_EXECUTABLE} -B ${_uml_arm64_streaming_generator}
        --loader ${_uml_arm64_streaming_loader}
        --table ${_uml_arm64_streaming_table}
        --assembly ${_uml_arm64_streaming_dir}/streaming_generated.inc
        --memory-assembly ${_uml_arm64_streaming_dir}/streaming_memory.inc
        --header ${_uml_arm64_streaming_dir}/streaming_forms.hpp
    DEPENDS ${_uml_arm64_streaming_generator} ${_uml_arm64_streaming_loader}
        ${_uml_arm64_streaming_table}
    COMMENT "Generating the arm64 streaming gadgets"
    VERBATIM)
set_source_files_properties(arm64/gadgets/engine.S PROPERTIES
    OBJECT_DEPENDS "${_uml_arm64_streaming_assembly}"
    INCLUDE_DIRECTORIES ${_uml_arm64_streaming_dir})
set_source_files_properties(arm64/gadgets/program_builder_streaming.cpp
    PROPERTIES INCLUDE_DIRECTORIES ${_uml_arm64_streaming_dir})
# The generated header for the tests that check the forms against the table.
add_library(uml_arm64_streaming_forms INTERFACE)
uml_declare_architecture_target(uml_arm64_streaming_forms cpu gadget)
target_include_directories(uml_arm64_streaming_forms INTERFACE
    ${_uml_arm64_streaming_dir})
set(_uml_arm64_gadget_sme_sources
    arm64/gadgets/program_builder_sme.cpp
    arm64/gadgets/program_builder_streaming.cpp
    arm64/gadgets/sme_helpers.cpp
    ${_uml_arm64_streaming_dir}/streaming_forms.hpp)

# Gadget-engine non-streaming SVE sources, owned by SVE lane V5: the SVE
# part's selection source and the gadget bodies and form header it generates
# at build time from the same encoding table. engine.S gains their
# OBJECT_DEPENDS and include directory by appending, so the SME block's
# lines stay as they are.
set(_uml_arm64_sve_dir ${CMAKE_CURRENT_BINARY_DIR}/arm64_sve)
set(_uml_arm64_sve_generator
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/gadgets/engine/generate_sve.py)
add_custom_command(
    OUTPUT ${_uml_arm64_sve_dir}/sve_generated.inc
        ${_uml_arm64_sve_dir}/sve_forms.hpp
    COMMAND ${Python3_EXECUTABLE} -B ${_uml_arm64_sve_generator}
        --loader ${_uml_arm64_streaming_loader}
        --table ${_uml_arm64_streaming_table}
        --assembly ${_uml_arm64_sve_dir}/sve_generated.inc
        --header ${_uml_arm64_sve_dir}/sve_forms.hpp
    DEPENDS ${_uml_arm64_sve_generator} ${_uml_arm64_streaming_loader}
        ${_uml_arm64_streaming_table}
    COMMENT "Generating the arm64 non-streaming SVE gadgets"
    VERBATIM)
set_property(SOURCE arm64/gadgets/engine.S APPEND PROPERTY
    OBJECT_DEPENDS ${_uml_arm64_sve_dir}/sve_generated.inc)
set_property(SOURCE arm64/gadgets/engine.S APPEND PROPERTY
    INCLUDE_DIRECTORIES ${_uml_arm64_sve_dir})
set_source_files_properties(arm64/gadgets/program_builder_sve.cpp
    PROPERTIES INCLUDE_DIRECTORIES ${_uml_arm64_sve_dir})
# The generated header for the tests that check the forms against the table.
add_library(uml_arm64_sve_forms INTERFACE)
uml_declare_architecture_target(uml_arm64_sve_forms cpu gadget)
target_include_directories(uml_arm64_sve_forms INTERFACE ${_uml_arm64_sve_dir})
set(_uml_arm64_gadget_sve_sources
    arm64/gadgets/program_builder_sve.cpp
    arm64/gadgets/sve_helpers.cpp
    ${_uml_arm64_sve_dir}/sve_forms.hpp)

# Gadget-engine AArch32 sources, owned by lane A32-G: the AArch32 part's
# gadget bodies and form tables the generator writes at build time from the
# decoder's encoding table (arm64/aarch32/a32_encodings.hpp), and the builder
# that translates A32 and T32 with the shared decoder. engine.S gains their
# OBJECT_DEPENDS and include directory by appending, so the blocks above stay
# as they are.
set(_uml_arm64_a32_dir ${CMAKE_CURRENT_BINARY_DIR}/arm64_a32)
set(_uml_arm64_a32_generator
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/gadgets/engine/generate_a32.py)
set(_uml_arm64_a32_table
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/aarch32/a32_encodings.hpp)
set(_uml_arm64_a32_table_rows
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/aarch32/a32_encodings.cpp)
set(_uml_arm64_a32_hand_written
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/gadgets/engine/a32.inc)
add_custom_command(
    OUTPUT ${_uml_arm64_a32_dir}/a32_generated.inc
        ${_uml_arm64_a32_dir}/a32_forms.hpp
    COMMAND ${Python3_EXECUTABLE} -B ${_uml_arm64_a32_generator}
        --table ${_uml_arm64_a32_table}
        --assembly ${_uml_arm64_a32_dir}/a32_generated.inc
        --header ${_uml_arm64_a32_dir}/a32_forms.hpp
    DEPENDS ${_uml_arm64_a32_generator}
        ${CMAKE_CURRENT_SOURCE_DIR}/arm64/gadgets/engine/a32_simd.py
        ${CMAKE_CURRENT_SOURCE_DIR}/arm64/gadgets/engine/a32_neon.py
        ${CMAKE_CURRENT_SOURCE_DIR}/arm64/gadgets/engine/a32_structure.py
        ${CMAKE_CURRENT_SOURCE_DIR}/arm64/gadgets/engine/a32_armv8.py
        ${_uml_arm64_a32_table}
        ${_uml_arm64_a32_table_rows} ${_uml_arm64_a32_hand_written}
    COMMENT "Generating the arm64 AArch32 gadgets"
    VERBATIM)
set_property(SOURCE arm64/gadgets/engine.S APPEND PROPERTY
    OBJECT_DEPENDS ${_uml_arm64_a32_dir}/a32_generated.inc)
set_property(SOURCE arm64/gadgets/engine.S APPEND PROPERTY
    INCLUDE_DIRECTORIES ${_uml_arm64_a32_dir})
set_source_files_properties(arm64/gadgets/a32/program_builder_a32.cpp
    arm64/gadgets/a32/program_builder_a32_vfp.cpp
    arm64/gadgets/a32/program_builder_a32_neon.cpp
    arm64/gadgets/a32/program_builder_a32_armv8.cpp
    PROPERTIES INCLUDE_DIRECTORIES ${_uml_arm64_a32_dir})
# The generated header for the tests that check the forms against the table.
add_library(uml_arm64_a32_forms INTERFACE)
uml_declare_architecture_target(uml_arm64_a32_forms cpu gadget)
target_include_directories(uml_arm64_a32_forms INTERFACE ${_uml_arm64_a32_dir})
set(_uml_arm64_gadget_a32_sources
    arm64/gadgets/a32/program_builder_a32.cpp
    arm64/gadgets/a32/program_builder_a32_vfp.cpp
    arm64/gadgets/a32/program_builder_a32_neon.cpp
    arm64/gadgets/a32/program_builder_a32_armv8.cpp
    arm64/gadgets/a32/library_helpers.cpp
    ${_uml_arm64_a32_dir}/a32_forms.hpp)

# The fetch window, not a limit on block length: only 0.07% of blocks run far
# enough to be cut off by it rather than ending at a branch.
set(UML_GADGET_BLOCK_INSTRUCTIONS "64" CACHE STRING
    "Guest instructions fetched per translation attempt")
add_library(uml_arm64_gadgets OBJECT
    arm64/gadgets/afp_twin.cpp
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
    arm64/gadgets/engine.S
    ${_uml_arm64_gadget_sme_sources}
    ${_uml_arm64_gadget_sve_sources}
    ${_uml_arm64_gadget_a32_sources})
uml_declare_architecture_target(uml_arm64_gadgets cpu gadget)
target_link_libraries(uml_arm64_gadgets PRIVATE UML::BridgeAbi UML::CpuContract)
target_link_libraries(uml_arm64_gadgets PUBLIC uml_arm64_execution_helpers
    uml_arm64_sve_semantics uml_arm64_aarch32_decoder
    uml_arm64_aarch32_semantics)
target_include_directories(uml_arm64_gadgets PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
target_link_libraries(uml_arm64_gadgets PUBLIC
    UML::GadgetHistogramConfiguration)

# Every image that carries uml_arm64_gadgets or uml_x86_64_gadgets links this,
# including those that compose their objects directly. It merges each engine's
# gadget region sections into __TEXT,__text and orders them first, the arm64
# region before the x86-64 one, so each region keeps its own bytes and its
# place whatever else the image holds (arm64/gadgets/engine.S and
# x86_64/gadgets/engine/region.inc).
add_library(uml_gadget_region_layout INTERFACE)
add_library(UML::GadgetRegionLayout ALIAS uml_gadget_region_layout)
uml_declare_architecture_target(uml_gadget_region_layout cpu gadget)
if(APPLE)
    set(_uml_gadget_region_order
        ${CMAKE_CURRENT_SOURCE_DIR}/gadget_region.order)
    set(_uml_gadget_region_options)
    foreach(_uml_gadget_region_section IN ITEMS
            __a64_bank __a64_forward __a64_vforward __a64_cold __a64_afp
            __a64_veneer __a64_sme __a64_sve __a32_bank __a32_cold __a32_veneer
            __x86_bank __x86_cold)
        list(APPEND _uml_gadget_region_options
            "LINKER:-rename_section,__TEXT,${_uml_gadget_region_section},__TEXT,__text")
    endforeach()
    target_link_options(uml_gadget_region_layout INTERFACE
        ${_uml_gadget_region_options}
        "LINKER:-order_file,${_uml_gadget_region_order}")
    set_property(TARGET uml_gadget_region_layout APPEND PROPERTY
        INTERFACE_LINK_DEPENDS ${_uml_gadget_region_order})
endif()

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
        set_target_properties(uml_x86_64_target uml_x86_64_gadgets PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
else()
    message(STATUS
        "gadget translation IPO unavailable: ${_uml_gadget_ipo_error}")
endif()

# JIT SME sources, owned by SME lane S4: the compiler's SME sources and the
# streaming-mode transitions in the first list (absolute paths, like the
# compiler's), executor-side sources in the second. The gadget engine's
# block above is disjoint.
set(_uml_cpu_jit_sme_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_compiler_sme.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_sme_transition.S)
set(_uml_cpu_jit_executor_sme_sources
    arm64/jit/jit_semantic_sme.cpp)
set(_uml_cpu_jit_executor_sve_sources
    arm64/jit/jit_semantic_sve.cpp)

# JIT non-streaming SVE sources, owned by SVE lane V4.
set(_uml_cpu_jit_sve_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_compiler_sve.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_compiler_sve_inline.cpp)

# The AArch32 compiler of the JIT (design 5), owned by lane A32-J.
set(_uml_cpu_jit_a32_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_compiler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_compiler_alu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_compiler_arith.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_compiler_armv8.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_compiler_cold.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_compiler_control.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_compiler_exclusive.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_compiler_memory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_compiler_vector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_compiler_vfp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_compiler_neon.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_compiler_neon_misc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_compiler_neon_shift.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_compiler_vmem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_operations.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_transfer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/a32/a32_vector_operations.cpp)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/arm64/jit/jit_tlb_translate.S
    ${_uml_cpu_jit_sme_sources}
    ${_uml_cpu_jit_sve_sources}
    ${_uml_cpu_jit_a32_sources})

add_library(uml_arm64_jit OBJECT ${_uml_cpu_jit_sources})
uml_declare_architecture_target(uml_arm64_jit cpu native)
target_link_libraries(uml_arm64_jit PRIVATE
    UML::BridgeAbi
    UML::CpuContract
    uml_cpu_executable_memory
    uml_arm64_aarch32_decoder
    uml_arm64_aarch32_semantics)
target_include_directories(uml_arm64_jit PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)

set(_uml_cpu_jit_executor_sources
    arm64/jit/a32/a32_execute.cpp
    arm64/jit/a32/a32_loop.cpp
    arm64/jit/jit_cache_geometry.cpp
    arm64/jit/jit_executor_a32.cpp
    arm64/jit/jit_executor_cache.cpp
    arm64/jit/jit_executor_execution.cpp
    arm64/jit/jit_executor_lifecycle.cpp
    arm64/jit/jit_executor_pool.cpp
    arm64/jit/jit_executor_linking.cpp
    arm64/jit/jit_linking.cpp
    arm64/jit/jit_publication_state.cpp
    arm64/jit/jit_semantic_atomic.cpp
    arm64/jit/jit_semantic_classes.cpp
    arm64/jit/jit_semantic_load_store.cpp
    arm64/jit/jit_semantic_memory.cpp
    arm64/jit/jit_semantic_reservation.cpp
    arm64/jit/jit_shared_translations.cpp
    ${_uml_cpu_jit_executor_sme_sources}
    ${_uml_cpu_jit_executor_sve_sources})

add_library(uml_arm64_jit_executor OBJECT
    ${_uml_cpu_jit_executor_sources})
uml_declare_architecture_target(uml_arm64_jit_executor cpu native)
target_link_libraries(uml_arm64_jit_executor PRIVATE
    UML::BridgeAbi
    UML::CpuContract)
target_link_libraries(uml_arm64_jit_executor PUBLIC
    uml_arm64_jit
    uml_arm64_execution_helpers
    uml_arm64_sve_semantics)
target_include_directories(uml_arm64_jit_executor PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_definitions(uml_arm64_jit_executor PRIVATE
    UML_JIT_RUNTIME_DIAGNOSTICS=0)

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
        UML::BridgeAbi
        UML::CpuContract)
    target_link_libraries(uml_arm64_jit_diagnostics_executor PUBLIC
        uml_arm64_jit
        uml_arm64_execution_helpers
        uml_arm64_sve_semantics)
    target_include_directories(uml_arm64_jit_diagnostics_executor PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_definitions(uml_arm64_jit_diagnostics_executor PRIVATE
        UML_JIT_RUNTIME_DIAGNOSTICS=1)
endif()

# Some CPU implementation translation units contain inline assembly using the
# same optional vocabulary as the standalone assembly sources. Keep that
# option confined to implementation targets that contain those bodies instead
# of widening code generation for every C++ target in an iOS package build.
if(UML_APPLE_CPU_INLINE_ASM_COMPILE_OPTION)
    set(_uml_inline_asm_targets
        uml_arm64_aarch32_semantics
        uml_arm64_gadgets
        uml_arm64_jit
        uml_arm64_jit_executor
        uml_arm64_sve_semantics)
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
        UML::BridgeAbi
        UML::CpuContract)
    target_link_libraries(uml_arm64_jit_test_support PUBLIC
        uml_arm64_jit
        uml_arm64_execution_helpers)
    target_include_directories(uml_arm64_jit_test_support PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
endif()

add_library(uml_arm64_gadget_executor OBJECT
    arm64/gadgets/executor.cpp)
uml_declare_architecture_target(uml_arm64_gadget_executor cpu gadget)
target_link_libraries(uml_arm64_gadget_executor PRIVATE
    UML::BridgeAbi
    UML::CpuContract)
target_link_libraries(uml_arm64_gadget_executor PUBLIC uml_arm64_gadgets)
target_include_directories(uml_arm64_gadget_executor PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_definitions(uml_arm64_gadget_executor PRIVATE
    UML_GADGET_CACHE_CENSUS=$<BOOL:${UML_GADGET_CACHE_CENSUS}>
    UML_GADGET_CACHE_SLOTS=${UML_GADGET_CACHE_SLOTS}
    UML_GADGET_BLOCK_INSTRUCTIONS=${UML_GADGET_BLOCK_INSTRUCTIONS}
    UML_GADGET_RESIDENT_GENERATIONS=${UML_GADGET_RESIDENT_GENERATIONS})

add_library(uml_arm64_gadget_engine OBJECT arm64/gadgets/engine_adapter.cpp)
uml_declare_architecture_target(uml_arm64_gadget_engine cpu gadget)
target_link_libraries(uml_arm64_gadget_engine PRIVATE
    UML::BridgeAbi
    UML::CpuContract)
target_link_libraries(uml_arm64_gadget_engine PUBLIC uml_arm64_gadget_executor)
target_include_directories(uml_arm64_gadget_engine PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)

add_library(uml_arm64_jit_engine OBJECT arm64/jit/engine_adapter.cpp)
uml_declare_architecture_target(uml_arm64_jit_engine cpu native)
target_link_libraries(uml_arm64_jit_engine PRIVATE
    UML::BridgeAbi
    UML::CpuContract)
target_link_libraries(uml_arm64_jit_engine PUBLIC
    uml_arm64_jit
    uml_arm64_jit_executor)
target_include_directories(uml_arm64_jit_engine PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)

add_library(uml_cpu_engine_factory OBJECT engine_factory.cpp)
uml_declare_architecture_target(uml_cpu_engine_factory cpu mixed)
target_link_libraries(uml_cpu_engine_factory PRIVATE
    UML::BridgeAbi
    UML::CpuContract)
target_link_libraries(uml_cpu_engine_factory PUBLIC
    uml_arm64_gadget_engine
    uml_arm64_jit_engine
    uml_arm64_execution_helpers)
target_include_directories(uml_cpu_engine_factory PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)

# OBJECT libraries avoid duplicate compilation, but CMake does not propagate
# their contents transitively. These link components expose the supported
# adapter graph for consumers that select one engine or the factory.
add_library(uml_cpu_jit_component INTERFACE)
uml_declare_architecture_target(uml_cpu_jit_component cpu native)
target_link_libraries(uml_cpu_jit_component INTERFACE
    UML::CpuContract
    UML::MemoryComponent)
target_sources(uml_cpu_jit_component INTERFACE
    $<TARGET_OBJECTS:uml_arm64_jit_engine>
    $<TARGET_OBJECTS:uml_cpu_executable_memory>
    $<TARGET_OBJECTS:uml_arm64_jit>
    $<TARGET_OBJECTS:uml_arm64_aarch32_decoder>
    $<TARGET_OBJECTS:uml_arm64_aarch32_semantics>
    $<TARGET_OBJECTS:uml_arm64_jit_executor>
    $<TARGET_OBJECTS:uml_arm64_sve_semantics>
    $<TARGET_OBJECTS:uml_arm64_execution_helpers>)

# The CPU-owned implementation carrier deliberately excludes memory objects.
# Production and test runtime compositions can pair these exact engine objects
# with the appropriate module-owned memory component without copying either
# module's source manifest.
add_library(uml_cpu_engine_objects INTERFACE)
add_library(UML::CpuEngineObjects ALIAS uml_cpu_engine_objects)
uml_declare_architecture_target(uml_cpu_engine_objects cpu mixed)
target_link_libraries(uml_cpu_engine_objects INTERFACE
    UML::CpuContract
    UML::GadgetRegionLayout)
target_sources(uml_cpu_engine_objects INTERFACE
    $<TARGET_OBJECTS:uml_cpu_engine_factory>
    $<TARGET_OBJECTS:uml_arm64_gadget_engine>
    $<TARGET_OBJECTS:uml_arm64_gadget_executor>
    $<TARGET_OBJECTS:uml_arm64_jit_engine>
    $<TARGET_OBJECTS:uml_cpu_executable_memory>
    $<TARGET_OBJECTS:uml_arm64_jit>
    $<TARGET_OBJECTS:uml_arm64_aarch32_decoder>
    $<TARGET_OBJECTS:uml_arm64_aarch32_semantics>
    $<TARGET_OBJECTS:uml_arm64_jit_executor>
    $<TARGET_OBJECTS:uml_arm64_gadgets>
    $<TARGET_OBJECTS:uml_arm64_aarch32_decoder>
    $<TARGET_OBJECTS:uml_arm64_aarch32_semantics>
    $<TARGET_OBJECTS:uml_arm64_sve_semantics>
    $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
if(TARGET uml_x86_64_gadgets)
    target_link_libraries(uml_cpu_engine_objects INTERFACE
        UML::ThirdPartyCephes128)
    target_sources(uml_cpu_engine_objects INTERFACE
        $<TARGET_OBJECTS:uml_x86_64_target>
        $<TARGET_OBJECTS:uml_x86_64_gadgets>)
endif()
# The x86-64 product's runtime seam dispatches to the macOS-only x86-64 JIT;
# the executable-memory owner it allocates through is already above.
if(UML_X86_64_JIT_RUNTIME)
    target_link_libraries(uml_cpu_engine_objects INTERFACE
        UML::ThirdPartyFEXCore)
    target_sources(uml_cpu_engine_objects INTERFACE
        $<TARGET_OBJECTS:uml_x86_64_jit>)
endif()

add_library(uml_cpu_engine_component INTERFACE)
uml_declare_architecture_target(uml_cpu_engine_component cpu mixed)
target_link_libraries(uml_cpu_engine_component INTERFACE
    UML::CpuEngineObjects
    UML::MemoryComponent)
