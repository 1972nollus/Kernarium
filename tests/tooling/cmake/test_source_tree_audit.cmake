# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED AUDIT_SCRIPT OR AUDIT_SCRIPT STREQUAL "")
    message(FATAL_ERROR "AUDIT_SCRIPT is required")
endif()
if(NOT EXISTS "${AUDIT_SCRIPT}")
    message(FATAL_ERROR "AUDIT_SCRIPT does not exist: ${AUDIT_SCRIPT}")
endif()
if(NOT DEFINED TEST_ROOT OR TEST_ROOT STREQUAL "")
    message(FATAL_ERROR "TEST_ROOT is required")
endif()

include("${AUDIT_SCRIPT}")

# The main audit proves the live tree. Exercise one mutation in each restored
# source-only contract family so removing the checks cannot silently leave only
# their positive fixtures behind.
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY
    "${TEST_ROOT}/darwin/cpu/arm64/gadgets"
    "${TEST_ROOT}/darwin/cpu/arm64/jit"
    "${TEST_ROOT}/tests/unit/arm64")

set(GADGET_FILES
    program_builder.cpp
    program_builder_decode.hpp
    program_builder_branch_system_impl.ipp
    program_builder_data_processing_impl.ipp
    program_builder_exception_impl.ipp
    program_builder_floating_simd_impl.ipp
    program_builder_load_store_impl.ipp)
foreach(FILE IN LISTS GADGET_FILES)
    configure_file(
        "${SOURCE_ROOT}/darwin/cpu/arm64/gadgets/${FILE}"
        "${TEST_ROOT}/darwin/cpu/arm64/gadgets/${FILE}"
        COPYONLY)
endforeach()

set(JIT_FILES
    jit_linking.hpp
    jit_publication.hpp
    jit_compiler.hpp
    jit_cache.hpp
    jit_compiler.cpp
    jit_semantic_memory.cpp
    jit_executor_execution.cpp
    jit_test_support.cpp)
foreach(FILE IN LISTS JIT_FILES)
    configure_file(
        "${SOURCE_ROOT}/darwin/cpu/arm64/jit/${FILE}"
        "${TEST_ROOT}/darwin/cpu/arm64/jit/${FILE}"
        COPYONLY)
endforeach()
configure_file(
    "${SOURCE_ROOT}/tests/unit/arm64/arm64_engine_test_support.cpp"
    "${TEST_ROOT}/tests/unit/arm64/arm64_engine_test_support.cpp"
    COPYONLY)

uml_audit_protected_engine_source_contracts(
    "${TEST_ROOT}" BASELINE_CONTRACT_VIOLATIONS)
if(BASELINE_CONTRACT_VIOLATIONS)
    list(JOIN BASELINE_CONTRACT_VIOLATIONS "\n  " FORMATTED_VIOLATIONS)
    message(FATAL_ERROR
        "protected engine source fixture is not initially valid:\n  ${FORMATTED_VIOLATIONS}")
endif()

set(COMPOSER "${TEST_ROOT}/darwin/cpu/arm64/gadgets/program_builder.cpp")
file(READ "${COMPOSER}" ORIGINAL_CONTENT)
string(REPLACE
    "#include \"program_builder_exception_impl.ipp\""
    "// removed by source-audit mutation fixture"
    MUTATED_CONTENT "${ORIGINAL_CONTENT}")
file(WRITE "${COMPOSER}" "${MUTATED_CONTENT}")
uml_audit_protected_engine_source_contracts(
    "${TEST_ROOT}" MUTATION_VIOLATIONS)
list(JOIN MUTATION_VIOLATIONS "\n" MUTATION_TEXT)
if(NOT MUTATION_TEXT MATCHES
   "G100 composer does not include exactly the five decode-family fragments")
    message(FATAL_ERROR "source audit missed the G100 composer mutation")
endif()
file(WRITE "${COMPOSER}" "${ORIGINAL_CONTENT}")

set(HARNESS
    "${TEST_ROOT}/tests/unit/arm64/arm64_engine_test_support.cpp")
file(READ "${HARNESS}" ORIGINAL_CONTENT)
string(REPLACE "gadget_indirect_miss" "mutated_indirect_miss"
    MUTATED_CONTENT "${ORIGINAL_CONTENT}")
file(WRITE "${HARNESS}" "${MUTATED_CONTENT}")
uml_audit_protected_engine_source_contracts(
    "${TEST_ROOT}" MUTATION_VIOLATIONS)
list(JOIN MUTATION_VIOLATIONS "\n" MUTATION_TEXT)
if(NOT MUTATION_TEXT MATCHES
   "common conformance does not reject leaked gadget_indirect_miss state")
    message(FATAL_ERROR "source audit missed the harness-boundary mutation")
endif()
file(WRITE "${HARNESS}" "${ORIGINAL_CONTENT}")

set(COMPILER_HEADER
    "${TEST_ROOT}/darwin/cpu/arm64/jit/jit_compiler.hpp")
file(READ "${COMPILER_HEADER}" ORIGINAL_CONTENT)
string(APPEND ORIGINAL_CONTENT_WITH_MUTATION
    "${ORIGINAL_CONTENT}\n#include \"jit_linking.hpp\"\n")
file(WRITE "${COMPILER_HEADER}" "${ORIGINAL_CONTENT_WITH_MUTATION}")
uml_audit_protected_engine_source_contracts(
    "${TEST_ROOT}" MUTATION_VIOLATIONS)
list(JOIN MUTATION_VIOLATIONS "\n" MUTATION_TEXT)
if(NOT MUTATION_TEXT MATCHES
   "translation interface depends on executor publication/link records")
    message(FATAL_ERROR "source audit missed the JIT-header mutation")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
