# SPDX-License-Identifier: GPL-2.0-or-later
# Static source ownership and layering audit.  This intentionally examines
# assembly and implementation includes as well as ordinary C++ headers.
if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

function(uml_collect_direct_quoted_includes PATH OUT_INCLUDES)
    if(NOT EXISTS "${PATH}")
        set(${OUT_INCLUDES} "" PARENT_SCOPE)
        return()
    endif()
    file(READ "${PATH}" CONTENT)
    string(REGEX MATCHALL
        "(^|\n)[ \t]*#[ \t]*include[ \t]+\"[^\"]+\""
        INCLUDE_DIRECTIVES "${CONTENT}")
    set(INCLUDES)
    foreach(INCLUDE_DIRECTIVE IN LISTS INCLUDE_DIRECTIVES)
        string(REGEX REPLACE
            ".*\"([^\"]+)\".*" "\\1" INCLUDE_NAME
            "${INCLUDE_DIRECTIVE}")
        list(APPEND INCLUDES "${INCLUDE_NAME}")
    endforeach()
    set(${OUT_INCLUDES} "${INCLUDES}" PARENT_SCOPE)
endfunction()

function(uml_audit_protected_engine_source_contracts ROOT OUT_VIOLATIONS)
    set(CONTRACT_VIOLATIONS)

    # G100 keeps the five measured decode families in one compiler-visible
    # translation unit without turning .ipp into a general implementation-
    # include convention. Check source ownership directly; object and archive
    # composition are intentionally outside this audit.
    set(GADGET_ROOT "${ROOT}/darwin/cpu/arm64/gadgets")
    set(GADGET_COMPOSER "${GADGET_ROOT}/program_builder.cpp")
    set(GADGET_INTERFACE "${GADGET_ROOT}/program_builder_decode.hpp")
    set(GADGET_FUNCTIONS
        decode_branch_system
        decode_data_processing
        decode_exception
        decode_floating_simd
        decode_load_store)
    set(GADGET_FRAGMENTS
        program_builder_branch_system_impl.ipp
        program_builder_data_processing_impl.ipp
        program_builder_exception_impl.ipp
        program_builder_floating_simd_impl.ipp
        program_builder_load_store_impl.ipp)

    file(GLOB_RECURSE DARWIN_IPP_FILES RELATIVE "${ROOT}"
        "${ROOT}/darwin/*.ipp")
    list(SORT DARWIN_IPP_FILES)
    set(EXPECTED_DARWIN_IPP_FILES)
    foreach(FRAGMENT IN LISTS GADGET_FRAGMENTS)
        list(APPEND EXPECTED_DARWIN_IPP_FILES
            "darwin/cpu/arm64/gadgets/${FRAGMENT}")
    endforeach()
    list(SORT EXPECTED_DARWIN_IPP_FILES)
    if(NOT "${DARWIN_IPP_FILES}" STREQUAL "${EXPECTED_DARWIN_IPP_FILES}")
        list(APPEND CONTRACT_VIOLATIONS
            "${GADGET_ROOT}: G100 permits exactly its five decode-family .ipp files")
    endif()

    if(NOT EXISTS "${GADGET_COMPOSER}")
        list(APPEND CONTRACT_VIOLATIONS
            "${GADGET_COMPOSER}: G100 composer is missing")
        set(GADGET_COMPOSER_CONTENT "")
        set(GADGET_COMPOSER_INCLUDES)
    else()
        file(READ "${GADGET_COMPOSER}" GADGET_COMPOSER_CONTENT)
        uml_collect_direct_quoted_includes(
            "${GADGET_COMPOSER}" GADGET_COMPOSER_INCLUDES)
    endif()
    if(NOT EXISTS "${GADGET_INTERFACE}")
        list(APPEND CONTRACT_VIOLATIONS
            "${GADGET_INTERFACE}: G100 decode interface is missing")
        set(GADGET_INTERFACE_CONTENT "")
    else()
        file(READ "${GADGET_INTERFACE}" GADGET_INTERFACE_CONTENT)
    endif()

    set(COMPOSED_GADGET_FRAGMENTS)
    foreach(INCLUDE_NAME IN LISTS GADGET_COMPOSER_INCLUDES)
        if(INCLUDE_NAME MATCHES
           "^program_builder_[A-Za-z0-9_]+_impl\\.ipp$")
            list(APPEND COMPOSED_GADGET_FRAGMENTS "${INCLUDE_NAME}")
        endif()
    endforeach()
    list(SORT COMPOSED_GADGET_FRAGMENTS)
    set(SORTED_GADGET_FRAGMENTS ${GADGET_FRAGMENTS})
    list(SORT SORTED_GADGET_FRAGMENTS)
    if(NOT "${COMPOSED_GADGET_FRAGMENTS}" STREQUAL
           "${SORTED_GADGET_FRAGMENTS}")
        list(APPEND CONTRACT_VIOLATIONS
            "${GADGET_COMPOSER}: G100 composer does not include exactly the five decode-family fragments")
    endif()

    list(LENGTH GADGET_FUNCTIONS GADGET_FUNCTION_COUNT)
    if(GADGET_FUNCTION_COUNT GREATER 0)
        math(EXPR GADGET_FUNCTION_LAST "${GADGET_FUNCTION_COUNT} - 1")
        foreach(INDEX RANGE 0 ${GADGET_FUNCTION_LAST})
            list(GET GADGET_FUNCTIONS ${INDEX} FUNCTION_NAME)
            list(GET GADGET_FRAGMENTS ${INDEX} FRAGMENT_NAME)
            set(FRAGMENT_PATH "${GADGET_ROOT}/${FRAGMENT_NAME}")
            set(DEFINITION_PATTERN
                "DecodeResult[ \t\r\n]+${FUNCTION_NAME}\\([ \t\r\n]*DecodeContext[ \t\r\n]+&context[ \t\r\n]*\\)[ \t\r\n]*[{]")
            set(DECLARATION_PATTERN
                "\\[\\[nodiscard\\]\\][ \t\r\n]+DecodeResult[ \t\r\n]+${FUNCTION_NAME}\\([ \t\r\n]*DecodeContext[ \t\r\n]+&context[ \t\r\n]*\\)[ \t\r\n]*;")
            if(NOT GADGET_INTERFACE_CONTENT MATCHES "${DECLARATION_PATTERN}")
                list(APPEND CONTRACT_VIOLATIONS
                    "${GADGET_INTERFACE}: G100 interface does not declare ${FUNCTION_NAME}")
            endif()
            if(GADGET_INTERFACE_CONTENT MATCHES "${DEFINITION_PATTERN}")
                list(APPEND CONTRACT_VIOLATIONS
                    "${GADGET_INTERFACE}: G100 interface implements ${FUNCTION_NAME}")
            endif()
            if(GADGET_COMPOSER_CONTENT MATCHES "${DEFINITION_PATTERN}")
                list(APPEND CONTRACT_VIOLATIONS
                    "${GADGET_COMPOSER}: G100 composer defines ${FUNCTION_NAME} directly")
            endif()
            if(NOT EXISTS "${FRAGMENT_PATH}")
                list(APPEND CONTRACT_VIOLATIONS
                    "${FRAGMENT_PATH}: G100 decode-family fragment is missing")
                continue()
            endif()
            file(READ "${FRAGMENT_PATH}" FRAGMENT_CONTENT)
            if(NOT FRAGMENT_CONTENT MATCHES "${DEFINITION_PATTERN}")
                list(APPEND CONTRACT_VIOLATIONS
                    "${FRAGMENT_PATH}: G100 fragment does not own ${FUNCTION_NAME}")
            endif()
            foreach(OTHER_FUNCTION IN LISTS GADGET_FUNCTIONS)
                if(OTHER_FUNCTION STREQUAL FUNCTION_NAME)
                    continue()
                endif()
                set(OTHER_DEFINITION_PATTERN
                    "DecodeResult[ \t\r\n]+${OTHER_FUNCTION}\\([ \t\r\n]*DecodeContext[ \t\r\n]+&context[ \t\r\n]*\\)[ \t\r\n]*[{]")
                if(FRAGMENT_CONTENT MATCHES "${OTHER_DEFINITION_PATTERN}")
                    list(APPEND CONTRACT_VIOLATIONS
                        "${FRAGMENT_PATH}: G100 fragment also defines ${OTHER_FUNCTION}")
                endif()
            endforeach()
        endforeach()
    endif()

    file(GLOB_RECURSE FIRST_PARTY_CXX_FILES LIST_DIRECTORIES false
        "${ROOT}/darwin/*.cpp"
        "${ROOT}/darwin/*.cc"
        "${ROOT}/darwin/*.cxx"
        "${ROOT}/darwin/*.h"
        "${ROOT}/darwin/*.hh"
        "${ROOT}/darwin/*.hpp"
        "${ROOT}/darwin/*.ipp"
        "${ROOT}/tests/*.cpp"
        "${ROOT}/tests/*.cc"
        "${ROOT}/tests/*.cxx"
        "${ROOT}/tests/*.h"
        "${ROOT}/tests/*.hh"
        "${ROOT}/tests/*.hpp"
        "${ROOT}/tests/*.ipp")
    foreach(PATH IN LISTS FIRST_PARTY_CXX_FILES)
        if(PATH STREQUAL GADGET_COMPOSER)
            continue()
        endif()
        file(READ "${PATH}" CONTENT)
        if(CONTENT MATCHES
           "(^|\n)[ \t]*#[ \t]*include[ \t]+[<\"]program_builder_[A-Za-z0-9_]+_impl\\.ipp[>\"]")
            list(APPEND CONTRACT_VIOLATIONS
                "${PATH}: G100 decode-family fragments may be included only by program_builder.cpp")
        endif()
    endforeach()

    # The common conformance harness observes leaked engine state; it must not
    # repair engine-specific state after the public run boundary.
    set(HARNESS
        "${ROOT}/tests/unit/arm64/arm64_engine_test_support.cpp")
    if(NOT EXISTS "${HARNESS}")
        list(APPEND CONTRACT_VIOLATIONS
            "${HARNESS}: common engine conformance harness is missing")
    else()
        file(READ "${HARNESS}" HARNESS_CONTENT)
        if(HARNESS_CONTENT MATCHES "restore_native_fp_environment")
            list(APPEND CONTRACT_VIOLATIONS
                "${HARNESS}: common conformance performs engine-specific state restoration")
        endif()
        foreach(MARKER IN ITEMS
                gadget_fp_active gadget_fpmr_active gadget_indirect_miss)
            if(NOT HARNESS_CONTENT MATCHES "${MARKER}")
                list(APPEND CONTRACT_VIOLATIONS
                    "${HARNESS}: common conformance does not reject leaked ${MARKER} state")
            endif()
        endforeach()
    endif()

    # Keep executor, publication, translation, and test contracts explicit by
    # auditing direct source includes rather than built object membership.
    set(JIT_ROOT "${ROOT}/darwin/cpu/arm64/jit")
    set(REQUIRED_JIT_FILES
        jit_linking.hpp
        jit_publication.hpp
        jit_compiler.hpp
        jit_cache.hpp
        jit_compiler.cpp
        jit_semantic_memory.cpp
        jit_executor_execution.cpp
        jit_test_support.cpp)
    foreach(JIT_FILE IN LISTS REQUIRED_JIT_FILES)
        if(NOT EXISTS "${JIT_ROOT}/${JIT_FILE}")
            list(APPEND CONTRACT_VIOLATIONS
                "${JIT_ROOT}/${JIT_FILE}: protected JIT source contract is missing")
        endif()
    endforeach()

    uml_collect_direct_quoted_includes(
        "${JIT_ROOT}/jit_linking.hpp" JIT_LINKING_INCLUDES)
    if(JIT_LINKING_INCLUDES)
        list(APPEND CONTRACT_VIOLATIONS
            "${JIT_ROOT}/jit_linking.hpp: link records have unreviewed direct dependencies: ${JIT_LINKING_INCLUDES}")
    endif()

    uml_collect_direct_quoted_includes(
        "${JIT_ROOT}/jit_publication.hpp" JIT_PUBLICATION_INCLUDES)
    if(NOT "cpu/arm64/cpu_state.hpp" IN_LIST JIT_PUBLICATION_INCLUDES OR
       "jit_linking.hpp" IN_LIST JIT_PUBLICATION_INCLUDES)
        list(APPEND CONTRACT_VIOLATIONS
            "${JIT_ROOT}/jit_publication.hpp: store publication must own CpuState directly without link records")
    endif()

    uml_collect_direct_quoted_includes(
        "${JIT_ROOT}/jit_compiler.hpp" JIT_COMPILER_INCLUDES)
    if("jit_linking.hpp" IN_LIST JIT_COMPILER_INCLUDES OR
       "jit_publication.hpp" IN_LIST JIT_COMPILER_INCLUDES)
        list(APPEND CONTRACT_VIOLATIONS
            "${JIT_ROOT}/jit_compiler.hpp: translation interface depends on executor publication/link records")
    endif()

    uml_collect_direct_quoted_includes(
        "${JIT_ROOT}/jit_cache.hpp" JIT_CACHE_INCLUDES)
    if(NOT "jit_linking.hpp" IN_LIST JIT_CACHE_INCLUDES)
        list(APPEND CONTRACT_VIOLATIONS
            "${JIT_ROOT}/jit_cache.hpp: cache does not own its link-record dependency directly")
    endif()

    set(JIT_BRIDGE_SOURCES
        jit_compiler.cpp
        jit_semantic_memory.cpp
        jit_executor_execution.cpp)
    set(JIT_BRIDGE_INCLUDES
        cpu/arm64/code_synchronization.hpp
        cpu/arm64/memory_transfer.hpp
        cpu/arm64/instructions/load_store_memory.hpp)
    foreach(INDEX RANGE 0 2)
        list(GET JIT_BRIDGE_SOURCES ${INDEX} JIT_BRIDGE_SOURCE)
        list(GET JIT_BRIDGE_INCLUDES ${INDEX} JIT_BRIDGE_INCLUDE)
        uml_collect_direct_quoted_includes(
            "${JIT_ROOT}/${JIT_BRIDGE_SOURCE}" JIT_SOURCE_INCLUDES)
        if(NOT "${JIT_BRIDGE_INCLUDE}" IN_LIST JIT_SOURCE_INCLUDES)
            list(APPEND CONTRACT_VIOLATIONS
                "${JIT_ROOT}/${JIT_BRIDGE_SOURCE}: relies transitively on ${JIT_BRIDGE_INCLUDE}")
        endif()
    endforeach()

    uml_collect_direct_quoted_includes(
        "${JIT_ROOT}/jit_test_support.cpp" JIT_TEST_SUPPORT_INCLUDES)
    foreach(REQUIRED_INCLUDE IN ITEMS jit_publication.hpp jit_test_api.hpp)
        if(NOT "${REQUIRED_INCLUDE}" IN_LIST JIT_TEST_SUPPORT_INCLUDES)
            list(APPEND CONTRACT_VIOLATIONS
                "${JIT_ROOT}/jit_test_support.cpp: test support does not include ${REQUIRED_INCLUDE} directly")
        endif()
    endforeach()

    set(${OUT_VIOLATIONS} "${CONTRACT_VIOLATIONS}" PARENT_SCOPE)
endfunction()

set(VIOLATIONS)

file(GLOB ROOT_C_SOURCES LIST_DIRECTORIES false
    "${SOURCE_ROOT}/*.c"
    "${SOURCE_ROOT}/Makefile")
set(C_SOURCES ${ROOT_C_SOURCES})
foreach(FIRST_PARTY_SCOPE IN ITEMS
        cmake darwin dependencies kernel scripts tests tools)
    if(NOT IS_DIRECTORY "${SOURCE_ROOT}/${FIRST_PARTY_SCOPE}")
        continue()
    endif()
    file(GLOB_RECURSE SCOPED_C_SOURCES LIST_DIRECTORIES false
        "${SOURCE_ROOT}/${FIRST_PARTY_SCOPE}/*.c"
        "${SOURCE_ROOT}/${FIRST_PARTY_SCOPE}/Makefile")
    list(APPEND C_SOURCES ${SCOPED_C_SOURCES})
endforeach()
list(REMOVE_DUPLICATES C_SOURCES)
foreach(PATH IN LISTS C_SOURCES)
    if(PATH STREQUAL "${SOURCE_ROOT}/tests/consumer/apple/main.c")
        # The installed-package acceptance intentionally includes one genuine
        # C translation unit. Production implementation remains C++-only.
        continue()
    endif()
    list(APPEND VIOLATIONS "${PATH}: project C source is not allowed")
endforeach()

set(LAYER_GLOBS)
foreach(EXTENSION IN ITEMS
        c cc cpp cxx m mm h hh hpp hxx ipp inl def S s asm inc)
    foreach(LAYER_DIRECTORY IN ITEMS cpu memory runtime device api app emulator platform)
        list(APPEND LAYER_GLOBS
            "${SOURCE_ROOT}/darwin/${LAYER_DIRECTORY}/*.${EXTENSION}")
    endforeach()
endforeach()
file(GLOB_RECURSE LAYER_SOURCES LIST_DIRECTORIES false ${LAYER_GLOBS})
foreach(PATH IN LISTS LAYER_SOURCES)
    file(READ "${PATH}" CONTENT)
    set(NORMALIZED_INCLUDES)
    string(REGEX MATCHALL
        "#[ \\t]*include[ \\t]*[\"<][^\">]+[\">]"
        INCLUDE_DIRECTIVES "${CONTENT}")
    get_filename_component(SOURCE_DIRECTORY "${PATH}" DIRECTORY)
    foreach(INCLUDE_DIRECTIVE IN LISTS INCLUDE_DIRECTIVES)
        string(REGEX REPLACE
            "^#[ \\t]*include[ \\t]*[\"<]([^\">]+)[\">]$"
            "\\1" INCLUDE_NAME "${INCLUDE_DIRECTIVE}")
        set(INCLUDE_CANDIDATES
            "${SOURCE_DIRECTORY}/${INCLUDE_NAME}"
            "${SOURCE_ROOT}/darwin/${INCLUDE_NAME}"
            "${SOURCE_ROOT}/darwin/include/${INCLUDE_NAME}")
        set(NORMALIZED_INCLUDE "${INCLUDE_NAME}")
        foreach(INCLUDE_CANDIDATE IN LISTS INCLUDE_CANDIDATES)
            cmake_path(NORMAL_PATH INCLUDE_CANDIDATE)
            if(EXISTS "${INCLUDE_CANDIDATE}")
                file(RELATIVE_PATH NORMALIZED_INCLUDE
                    "${SOURCE_ROOT}" "${INCLUDE_CANDIDATE}")
                break()
            endif()
        endforeach()
        list(APPEND NORMALIZED_INCLUDES "${NORMALIZED_INCLUDE}")
    endforeach()
    string(JOIN "\n" NORMALIZED_INCLUDE_TEXT ${NORMALIZED_INCLUDES})
    if(PATH MATCHES "/darwin/cpu/")
        if(NORMALIZED_INCLUDE_TEXT MATCHES
           "(^|\n)(darwin/)?(runtime|device|app)/")
            list(APPEND VIOLATIONS
                "${PATH}: CPU layer includes runtime, device, or application implementation")
        endif()
    endif()
    if(PATH MATCHES "/darwin/memory/")
        if(NORMALIZED_INCLUDE_TEXT MATCHES
           "(^|\n)(darwin/)?(runtime|device|app)/")
            list(APPEND VIOLATIONS
                "${PATH}: memory layer includes runtime, device, or application implementation")
        endif()
    endif()
    if(PATH MATCHES "/darwin/device/")
        if(NORMALIZED_INCLUDE_TEXT MATCHES
           "(^|\n)(darwin/)?(runtime|app)/")
            list(APPEND VIOLATIONS
                "${PATH}: device layer includes runtime or application implementation")
        endif()
    endif()
    if(PATH MATCHES "/darwin/platform/" AND
       NORMALIZED_INCLUDE_TEXT MATCHES "(^|\n)(darwin/)?cpu/")
        list(APPEND VIOLATIONS
            "${PATH}: platform layer includes a CPU-owned contract")
    endif()
    if(PATH MATCHES "/darwin/cpu/arm64/interpreter/")
        if(NORMALIZED_INCLUDE_TEXT MATCHES
           "(^|\n)(darwin/)?cpu/arm64/(gadgets|jit)/")
            list(APPEND VIOLATIONS
                "${PATH}: interpreter includes another engine implementation")
        endif()
    elseif(PATH MATCHES "/darwin/cpu/arm64/gadgets/")
        if(NORMALIZED_INCLUDE_TEXT MATCHES
           "(^|\n)(darwin/)?cpu/arm64/(interpreter|jit)/")
            list(APPEND VIOLATIONS
                "${PATH}: gadget engine includes another engine implementation")
        endif()
    elseif(PATH MATCHES "/darwin/cpu/arm64/jit/")
        if(NORMALIZED_INCLUDE_TEXT MATCHES
           "(^|\n)(darwin/)?cpu/arm64/(interpreter|gadgets)/")
            list(APPEND VIOLATIONS
                "${PATH}: JIT includes another engine implementation")
        endif()
    endif()
    # Sandboxed iOS can execute the interpreter and precompiled gadget engine
    # without a dynamic-code entitlement. Keep executable allocation and
    # publication primitives confined to the explicitly selected JIT module;
    # merely avoiding a simultaneous W+X mapping is not sufficient because an
    # RW-to-RX transition is also dynamic code publication.
    if(PATH MATCHES "/darwin/cpu/arm64/(interpreter|gadgets)/" AND
       CONTENT MATCHES
       "(MAP_JIT|PROT_EXEC|VM_PROT_EXECUTE|mprotect[ \t]*\\(|mach_vm_protect[ \t]*\\(|vm_protect[ \t]*\\(|pthread_jit_write_protect|sys_icache_invalidate[ \t]*\\()")
        list(APPEND VIOLATIONS
            "${PATH}: non-JIT engine uses an executable-memory or code-publication primitive")
    endif()
    if(PATH MATCHES "/darwin/cpu/" AND
       NOT PATH MATCHES "/darwin/cpu/arm64/jit/jit_arena\.cpp$" AND
       CONTENT MATCHES
       "(MAP_JIT|PROT_EXEC|VM_PROT_EXECUTE|mprotect[ \t]*\\(|mach_vm_protect[ \t]*\\(|vm_protect[ \t]*\\(|pthread_jit_write_protect|sys_icache_invalidate[ \t]*\\()")
        list(APPEND VIOLATIONS
            "${PATH}: CPU executable-memory primitives must be owned by the JIT arena")
    endif()
endforeach()

# C/C++ may not include implementation fragments.  Assembly deliberately uses
# verified .inc contracts, so it is covered by the layer audit above instead.
file(GLOB_RECURSE CXX_SOURCES LIST_DIRECTORIES false
    "${SOURCE_ROOT}/darwin/*.cpp"
    "${SOURCE_ROOT}/darwin/*.cc"
    "${SOURCE_ROOT}/darwin/*.cxx"
    "${SOURCE_ROOT}/darwin/*.hpp"
    "${SOURCE_ROOT}/darwin/*.h"
    "${SOURCE_ROOT}/darwin/*.hh"
    "${SOURCE_ROOT}/tests/*.cpp"
    "${SOURCE_ROOT}/tests/*.cc"
    "${SOURCE_ROOT}/tests/*.cxx"
    "${SOURCE_ROOT}/tests/*.hpp"
    "${SOURCE_ROOT}/tests/*.h")
foreach(PATH IN LISTS CXX_SOURCES)
    file(READ "${PATH}" CONTENT)
    if(CONTENT MATCHES
       "(^|\\n)[ \\t]*#[ \\t]*include[ \\t]+[^\\n]*(\\.(cpp|cc|cxx|inc)|_impl\\.hpp)[\">]")
        list(APPEND VIOLATIONS
            "${PATH}: C/C++ source includes an implementation fragment")
    endif()
    if(PATH MATCHES "/darwin/cpu/.*\\.(h|hh|hpp|hxx)$" AND
       CONTENT MATCHES "(^|\\n)[ \\t]*namespace[ \\t]*\\{")
        list(APPEND VIOLATIONS
            "${PATH}: CPU header creates anonymous per-translation-unit ownership")
    endif()
endforeach()

uml_audit_protected_engine_source_contracts(
    "${SOURCE_ROOT}" PROTECTED_ENGINE_SOURCE_VIOLATIONS)
list(APPEND VIOLATIONS ${PROTECTED_ENGINE_SOURCE_VIOLATIONS})

# All first-party CTest registrations must pass through the suite helpers so
# manifest ownership, labels, fixture policy, and aggregate binary dependencies
# cannot be bypassed by a raw add_test() in another directory.
include("${SOURCE_ROOT}/cmake/TestRegistrationPolicy.cmake")
uml_find_raw_test_registrations(
    SOURCE_ROOT "${SOURCE_ROOT}"
    OUT_VIOLATIONS RAW_TEST_REGISTRATION_VIOLATIONS)
list(APPEND VIOLATIONS ${RAW_TEST_REGISTRATION_VIOLATIONS})

file(GLOB_RECURSE UNIT_SCRIPT_RUNNERS LIST_DIRECTORIES false
    "${SOURCE_ROOT}/tests/unit/*.py"
    "${SOURCE_ROOT}/tests/unit/*.sh")
foreach(PATH IN LISTS UNIT_SCRIPT_RUNNERS)
    list(APPEND VIOLATIONS
        "${PATH}: Python and shell are not allowed in the host unit-test tree")
endforeach()

# Guest domains and performance runners may share process/console mechanics
# only through tests/integration/support. Importing another suite's entry point
# couples its command-line policy and teardown behavior into an unrelated test.
file(GLOB_RECURSE GUEST_RUNNER_PYTHON LIST_DIRECTORIES false
    "${SOURCE_ROOT}/tests/integration/*.py"
    "${SOURCE_ROOT}/tests/perf/*.py")
foreach(PATH IN LISTS GUEST_RUNNER_PYTHON)
    file(READ "${PATH}" CONTENT)
    if(CONTENT MATCHES
       "(^|\n)[ \t]*(from|import)[ \t]+((tests\.)?integration\.)?run_guest_(ltp|smp|jit|network|gpu)")
        list(APPEND VIOLATIONS
            "${PATH}: guest suites must share helpers through tests/integration/support")
    endif()
endforeach()

# Standalone assembly benchmarks are intentionally outside CTest, but they are
# still maintained source. Require an exact inventory, an explicit runner and
# retention purpose, and a canonical opt-in build target so probes cannot turn
# into anonymous debris after an optimization experiment ends.
set(PERFORMANCE_PROBE_ROOT "${SOURCE_ROOT}/tests/perf")
set(PERFORMANCE_PROBE_MANIFEST "${PERFORMANCE_PROBE_ROOT}/probes.tsv")
set(PERFORMANCE_PROBE_CMAKE "${PERFORMANCE_PROBE_ROOT}/CMakeLists.txt")
file(GLOB PERFORMANCE_PROBE_SOURCES RELATIVE "${PERFORMANCE_PROBE_ROOT}"
    "${PERFORMANCE_PROBE_ROOT}/*.S")
list(SORT PERFORMANCE_PROBE_SOURCES)
set(PERFORMANCE_PROBE_MANIFEST_SOURCES)
if(NOT EXISTS "${PERFORMANCE_PROBE_MANIFEST}")
    list(APPEND VIOLATIONS
        "${PERFORMANCE_PROBE_MANIFEST}: performance probe inventory is missing")
else()
    file(STRINGS "${PERFORMANCE_PROBE_MANIFEST}" PERFORMANCE_PROBE_ROWS)
    if(PERFORMANCE_PROBE_ROWS)
        list(POP_FRONT PERFORMANCE_PROBE_ROWS PERFORMANCE_PROBE_HEADER)
    else()
        set(PERFORMANCE_PROBE_HEADER "")
    endif()
    if(NOT PERFORMANCE_PROBE_HEADER STREQUAL
       "source\tcategory\trequired_feature\trunner\tretention")
        list(APPEND VIOLATIONS
            "${PERFORMANCE_PROBE_MANIFEST}: invalid performance probe header")
    endif()
    foreach(PERFORMANCE_PROBE_ROW IN LISTS PERFORMANCE_PROBE_ROWS)
        if(PERFORMANCE_PROBE_ROW STREQUAL "" OR
           PERFORMANCE_PROBE_ROW MATCHES "^#")
            continue()
        endif()
        string(REPLACE "\t" ";" PERFORMANCE_PROBE_FIELDS
            "${PERFORMANCE_PROBE_ROW}")
        list(LENGTH PERFORMANCE_PROBE_FIELDS PERFORMANCE_PROBE_FIELD_COUNT)
        if(NOT PERFORMANCE_PROBE_FIELD_COUNT EQUAL 5)
            list(APPEND VIOLATIONS
                "${PERFORMANCE_PROBE_MANIFEST}: malformed row '${PERFORMANCE_PROBE_ROW}'")
            continue()
        endif()
        list(GET PERFORMANCE_PROBE_FIELDS 0 PERFORMANCE_PROBE_SOURCE)
        list(GET PERFORMANCE_PROBE_FIELDS 1 PERFORMANCE_PROBE_CATEGORY)
        list(GET PERFORMANCE_PROBE_FIELDS 2 PERFORMANCE_PROBE_FEATURE)
        list(GET PERFORMANCE_PROBE_FIELDS 3 PERFORMANCE_PROBE_RUNNER)
        list(GET PERFORMANCE_PROBE_FIELDS 4 PERFORMANCE_PROBE_RETENTION)
        list(FIND PERFORMANCE_PROBE_MANIFEST_SOURCES
            "${PERFORMANCE_PROBE_SOURCE}" PERFORMANCE_PROBE_DUPLICATE)
        if(NOT PERFORMANCE_PROBE_DUPLICATE EQUAL -1)
            list(APPEND VIOLATIONS
                "${PERFORMANCE_PROBE_MANIFEST}: duplicate probe ${PERFORMANCE_PROBE_SOURCE}")
        else()
            list(APPEND PERFORMANCE_PROBE_MANIFEST_SOURCES
                "${PERFORMANCE_PROBE_SOURCE}")
        endif()
        if(NOT PERFORMANCE_PROBE_SOURCE MATCHES
           "^guest_[a-z0-9_]+\\.S$" OR
           NOT EXISTS "${PERFORMANCE_PROBE_ROOT}/${PERFORMANCE_PROBE_SOURCE}")
            list(APPEND VIOLATIONS
                "${PERFORMANCE_PROBE_MANIFEST}: invalid probe source ${PERFORMANCE_PROBE_SOURCE}")
        endif()
        if(NOT PERFORMANCE_PROBE_CATEGORY MATCHES "^[a-z][a-z0-9-]+$")
            list(APPEND VIOLATIONS
                "${PERFORMANCE_PROBE_MANIFEST}: invalid category for ${PERFORMANCE_PROBE_SOURCE}")
        endif()
        if(NOT PERFORMANCE_PROBE_FEATURE MATCHES "^(base|lse)$")
            list(APPEND VIOLATIONS
                "${PERFORMANCE_PROBE_MANIFEST}: invalid feature for ${PERFORMANCE_PROBE_SOURCE}")
        endif()
        if(NOT PERFORMANCE_PROBE_RUNNER STREQUAL
           "run_guest_link_bench.py" OR
           NOT EXISTS "${PERFORMANCE_PROBE_ROOT}/${PERFORMANCE_PROBE_RUNNER}")
            list(APPEND VIOLATIONS
                "${PERFORMANCE_PROBE_MANIFEST}: invalid runner for ${PERFORMANCE_PROBE_SOURCE}")
        endif()
        if(PERFORMANCE_PROBE_RETENTION STREQUAL "")
            list(APPEND VIOLATIONS
                "${PERFORMANCE_PROBE_MANIFEST}: missing retention rationale for ${PERFORMANCE_PROBE_SOURCE}")
        endif()
        if(EXISTS "${PERFORMANCE_PROBE_ROOT}/${PERFORMANCE_PROBE_SOURCE}")
            file(READ "${PERFORMANCE_PROBE_ROOT}/${PERFORMANCE_PROBE_SOURCE}"
                PERFORMANCE_PROBE_CONTENT)
            if(NOT PERFORMANCE_PROBE_CONTENT MATCHES
               "(^|\n)[ \t]*\\.global[ \t]+_start" OR
               NOT PERFORMANCE_PROBE_CONTENT MATCHES "success_message")
                list(APPEND VIOLATIONS
                    "${PERFORMANCE_PROBE_ROOT}/${PERFORMANCE_PROBE_SOURCE}: probe lacks the self-checking freestanding contract")
            endif()
        endif()
    endforeach()
endif()
list(SORT PERFORMANCE_PROBE_MANIFEST_SOURCES)
if(NOT PERFORMANCE_PROBE_SOURCES STREQUAL
   PERFORMANCE_PROBE_MANIFEST_SOURCES)
    list(APPEND VIOLATIONS
        "${PERFORMANCE_PROBE_MANIFEST}: inventory does not exactly own every tests/perf assembly probe")
endif()
if(NOT EXISTS "${PERFORMANCE_PROBE_CMAKE}")
    list(APPEND VIOLATIONS
        "${PERFORMANCE_PROBE_CMAKE}: performance probe build owner is missing")
else()
    file(READ "${PERFORMANCE_PROBE_CMAKE}" PERFORMANCE_PROBE_CMAKE_CONTENT)
    if(NOT PERFORMANCE_PROBE_CMAKE_CONTENT MATCHES
       "performance-probe-binaries" OR
       NOT PERFORMANCE_PROBE_CMAKE_CONTENT MATCHES "probes\\.tsv")
        list(APPEND VIOLATIONS
            "${PERFORMANCE_PROBE_CMAKE}: performance probe build owner is incomplete")
    endif()
endif()

# CTest already exports PYTHONDONTWRITEBYTECODE, but the same runners are also
# invoked directly by developers and release scripts.  Make standalone
# no-cache behavior a source-level contract, and reject any cache directory
# that has already leaked into a first-party runner tree.
file(GLOB_RECURSE FIRST_PARTY_PYTHON LIST_DIRECTORIES false
	"${SOURCE_ROOT}/dependencies/*.py"
	"${SOURCE_ROOT}/kernel/*.py"
    "${SOURCE_ROOT}/scripts/*.py"
    "${SOURCE_ROOT}/tests/*.py"
    "${SOURCE_ROOT}/tools/*.py")
foreach(PATH IN LISTS FIRST_PARTY_PYTHON)
    file(READ "${PATH}" CONTENT)
    if(NOT CONTENT MATCHES "dont_write_bytecode[ \t]*=[ \t]*True")
        list(APPEND VIOLATIONS
            "${PATH}: standalone Python runner does not disable bytecode caches")
    endif()
endforeach()
file(GLOB_RECURSE FIRST_PARTY_ENTRIES LIST_DIRECTORIES true
	"${SOURCE_ROOT}/dependencies/*"
	"${SOURCE_ROOT}/kernel/*"
    "${SOURCE_ROOT}/scripts/*"
    "${SOURCE_ROOT}/tests/*"
    "${SOURCE_ROOT}/tools/*")
foreach(PATH IN LISTS FIRST_PARTY_ENTRIES)
    if(IS_DIRECTORY "${PATH}" AND PATH MATCHES "/__pycache__$")
        list(APPEND VIOLATIONS
            "${PATH}: source-adjacent Python bytecode cache is forbidden")
    endif()
endforeach()

# One-off investigation inputs, probe binaries, source clones, and superseded
# dependency prefixes are not repository inputs or durable evidence.
set(FORBIDDEN_DEBRIS
    "${SOURCE_ROOT}/.DS_Store"
    "${SOURCE_ROOT}/external/libvncserver"
    "${SOURCE_ROOT}/external/virglrenderer"
    "${SOURCE_ROOT}/tmp"
    "${SOURCE_ROOT}/.build/darwin/network-probe"
    "${SOURCE_ROOT}/.build/run_renderer_probe.py")
file(GLOB ROOT_PDF_DEBRIS LIST_DIRECTORIES false "${SOURCE_ROOT}/*.pdf")
file(GLOB PREFIX_BACKUP_DEBRIS LIST_DIRECTORIES true
    "${SOURCE_ROOT}/.build/dependencies/*.pre-*")
list(APPEND FORBIDDEN_DEBRIS ${ROOT_PDF_DEBRIS} ${PREFIX_BACKUP_DEBRIS})
foreach(PATH IN LISTS FORBIDDEN_DEBRIS)
    if(EXISTS "${PATH}" OR IS_SYMLINK "${PATH}")
        list(APPEND VIOLATIONS "${PATH}: obsolete ignored audit debris")
    endif()
endforeach()

# Verify that every implementation source has a first-party CMake owner using
# its complete module-relative path. A same-named source in another module is
# not evidence of ownership.
file(GLOB_RECURSE IMPLEMENTATION_SOURCES LIST_DIRECTORIES false
    "${SOURCE_ROOT}/darwin/*.cpp"
    "${SOURCE_ROOT}/darwin/*.cc"
    "${SOURCE_ROOT}/darwin/*.cxx"
    "${SOURCE_ROOT}/darwin/*.m"
    "${SOURCE_ROOT}/darwin/*.mm"
    "${SOURCE_ROOT}/darwin/*.S"
    "${SOURCE_ROOT}/darwin/*.s")
file(GLOB_RECURSE OWNERSHIP_MANIFESTS LIST_DIRECTORIES false
    "${SOURCE_ROOT}/darwin/CMakeLists.txt")
foreach(PATH IN LISTS IMPLEMENTATION_SOURCES)
    file(RELATIVE_PATH SOURCE_RELATIVE_PATH "${SOURCE_ROOT}" "${PATH}")
    set(OWNED FALSE)
    foreach(MANIFEST IN LISTS OWNERSHIP_MANIFESTS)
        get_filename_component(MANIFEST_DIRECTORY "${MANIFEST}" DIRECTORY)
        file(RELATIVE_PATH MANIFEST_RELATIVE_PATH
            "${MANIFEST_DIRECTORY}" "${PATH}")
        file(READ "${MANIFEST}" MANIFEST_TEXT)
        string(REGEX REPLACE "#[^\n]*" "" MANIFEST_CODE "${MANIFEST_TEXT}")
        string(FIND "${MANIFEST_CODE}" "${MANIFEST_RELATIVE_PATH}"
            RELATIVE_OWNER_INDEX)
        string(FIND "${MANIFEST_CODE}" "${SOURCE_RELATIVE_PATH}"
            SOURCE_OWNER_INDEX)
        if(NOT RELATIVE_OWNER_INDEX EQUAL -1 OR
           NOT SOURCE_OWNER_INDEX EQUAL -1)
            set(OWNED TRUE)
            break()
        endif()
    endforeach()
    if(NOT OWNED)
        list(APPEND VIOLATIONS
            "${PATH}: implementation source has no CMake owner")
    endif()
endforeach()

# Empty architecture directories tend to leave behind misleading ownership
# claims after a move.  Headers, source, or the directory CMake manifest all
# count as substantive ownership.
file(GLOB_RECURSE ARCHITECTURE_ENTRIES LIST_DIRECTORIES true
    "${SOURCE_ROOT}/darwin/*")
foreach(FULL_DIRECTORY IN LISTS ARCHITECTURE_ENTRIES)
    if(IS_DIRECTORY "${FULL_DIRECTORY}")
        file(GLOB DIRECTORY_CHILDREN LIST_DIRECTORIES true
            "${FULL_DIRECTORY}/*")
        if(NOT DIRECTORY_CHILDREN)
            list(APPEND VIOLATIONS
                "${FULL_DIRECTORY}: empty residual architecture directory")
        endif()
    endif()
endforeach()

# Validate concrete paths named by the architecture guide. Markdown links are
# resolved relative to the document; inline module paths are resolved first
# below darwin/ and then from the repository root.
set(ARCHITECTURE_DOCUMENT "${SOURCE_ROOT}/docs/architecture.md")
file(READ "${ARCHITECTURE_DOCUMENT}" ARCHITECTURE_TEXT)
file(STRINGS "${ARCHITECTURE_DOCUMENT}" ARCHITECTURE_LINES)
foreach(DOCUMENT_LINE IN LISTS ARCHITECTURE_LINES)
    if(DOCUMENT_LINE MATCHES "\\]\\(([^)]+)\\)")
        set(REFERENCE "${CMAKE_MATCH_1}")
        if(REFERENCE MATCHES "^(https?:|#)")
            continue()
        endif()
        set(CANDIDATE "${SOURCE_ROOT}/docs/${REFERENCE}")
        cmake_path(NORMAL_PATH CANDIDATE)
        if(NOT EXISTS "${CANDIDATE}")
            list(APPEND VIOLATIONS
                "${ARCHITECTURE_DOCUMENT}: broken Markdown path ${REFERENCE}")
        endif()
    endif()
endforeach()
string(REGEX MATCHALL
    "`[A-Za-z0-9_./-]+\\.(h|hpp|cpp|cmake|tsv|md|txt)`"
    ARCHITECTURE_INLINE_PATHS "${ARCHITECTURE_TEXT}")
foreach(QUOTED_PATH IN LISTS ARCHITECTURE_INLINE_PATHS)
    string(REGEX REPLACE "^`([^`]+)`$" "\\1" REFERENCE "${QUOTED_PATH}")
    set(CANDIDATES
        "${SOURCE_ROOT}/darwin/${REFERENCE}"
        "${SOURCE_ROOT}/${REFERENCE}"
        "${SOURCE_ROOT}/docs/${REFERENCE}")
    set(REFERENCE_EXISTS FALSE)
    foreach(CANDIDATE IN LISTS CANDIDATES)
        cmake_path(NORMAL_PATH CANDIDATE)
        if(EXISTS "${CANDIDATE}")
            set(REFERENCE_EXISTS TRUE)
            break()
        endif()
    endforeach()
    if(NOT REFERENCE_EXISTS)
        list(APPEND VIOLATIONS
            "${ARCHITECTURE_DOCUMENT}: stale inline path ${REFERENCE}")
    endif()
endforeach()

include("${SOURCE_ROOT}/cmake/SourceSizeReport.cmake")
set(SOURCE_SIZE_ARGUMENTS
    SOURCE_ROOT "${SOURCE_ROOT}"
    REVIEW_MANIFEST "${SOURCE_ROOT}/docs/source-size-reviews.tsv"
    OUT_ERRORS SOURCE_SIZE_METADATA_ERRORS)
if(DEFINED SOURCE_SIZE_REPORT AND NOT SOURCE_SIZE_REPORT STREQUAL "")
    list(APPEND SOURCE_SIZE_ARGUMENTS REPORT_PATH "${SOURCE_SIZE_REPORT}")
endif()
uml_report_source_sizes(${SOURCE_SIZE_ARGUMENTS})
foreach(SOURCE_SIZE_METADATA_ERROR IN LISTS SOURCE_SIZE_METADATA_ERRORS)
    list(APPEND VIOLATIONS
        "source-size review metadata: ${SOURCE_SIZE_METADATA_ERROR}")
endforeach()

# Keep project documentation together. The root retains only the agent guide,
# its compatibility link, and the conventional human-facing README.
file(GLOB ROOT_MARKDOWN_DOCUMENTS LIST_DIRECTORIES false
    "${SOURCE_ROOT}/*.md")
set(ROOT_MARKDOWN_ALLOWLIST
    AGENTS.md
    CLAUDE.md
    README.md)
foreach(ROOT_MARKDOWN_DOCUMENT IN LISTS ROOT_MARKDOWN_DOCUMENTS)
    get_filename_component(ROOT_MARKDOWN_NAME
        "${ROOT_MARKDOWN_DOCUMENT}" NAME)
    if(NOT ROOT_MARKDOWN_NAME IN_LIST ROOT_MARKDOWN_ALLOWLIST)
        list(APPEND VIOLATIONS
            "${ROOT_MARKDOWN_DOCUMENT}: project documentation belongs under docs/")
    endif()
endforeach()

# Active implementation/docs must use the neutral module vocabulary. Historical
# planning and evidence archives are deliberately excluded from this terminology
# check even though they now live with the active documentation under docs/.
file(GLOB_RECURSE TERMINOLOGY_FILES LIST_DIRECTORIES false
    "${SOURCE_ROOT}/darwin/*"
    "${SOURCE_ROOT}/cmake/*"
    "${SOURCE_ROOT}/docs/*.md")
list(REMOVE_ITEM TERMINOLOGY_FILES
    "${SOURCE_ROOT}/docs/HOT_PATH_EXPERIMENTS.md"
    "${SOURCE_ROOT}/docs/REFACTORING_PLAN.md"
    "${SOURCE_ROOT}/docs/REFACTOR_HANDOFF.md")
foreach(PATH IN LISTS TERMINOLOGY_FILES)
    if(IS_DIRECTORY "${PATH}")
        continue()
    endif()
    file(READ "${PATH}" CONTENT)
    if(CONTENT MATCHES "darwin/backend|darwin/frontend")
        list(APPEND VIOLATIONS
            "${PATH}: active documentation/source uses the retired backend/frontend layout")
    endif()
endforeach()

if(VIOLATIONS)
    list(JOIN VIOLATIONS "\n  " FORMATTED_VIOLATIONS)
    message(FATAL_ERROR
        "Host source-tree architecture audit failed:\n  ${FORMATTED_VIOLATIONS}")
endif()
