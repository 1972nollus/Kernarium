# SPDX-License-Identifier: GPL-2.0-or-later
# Static source ownership and layering audit.  This intentionally examines
# assembly and implementation includes as well as ordinary C++ headers.
if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

# The gadget composer is the one C++ file that includes implementation
# fragments: its .ipp decode families stay in one compiler-visible translation
# unit without making .ipp a general convention.
# A darwin/ source that includes a header existing only under tests/ compiles
# only when a test target adds that directory to its include path.
function(uml_audit_test_only_includes ROOT OUT_VIOLATIONS)
    file(GLOB_RECURSE TEST_HEADERS LIST_DIRECTORIES false
        "${ROOT}/tests/*.h" "${ROOT}/tests/*.hpp")
    set(DARWIN_GLOBS)
    foreach(EXTENSION IN ITEMS c cc cpp cxx m mm h hh hpp hxx ipp inl inc S)
        list(APPEND DARWIN_GLOBS "${ROOT}/darwin/*.${EXTENSION}")
    endforeach()
    file(GLOB_RECURSE DARWIN_FILES LIST_DIRECTORIES false ${DARWIN_GLOBS})
    set(TEST_ONLY_NAMES)
    foreach(PATH IN LISTS TEST_HEADERS)
        get_filename_component(HEADER_NAME "${PATH}" NAME)
        list(APPEND TEST_ONLY_NAMES "${HEADER_NAME}")
    endforeach()
    foreach(PATH IN LISTS DARWIN_FILES)
        get_filename_component(HEADER_NAME "${PATH}" NAME)
        list(REMOVE_ITEM TEST_ONLY_NAMES "${HEADER_NAME}")
    endforeach()
    set(FOUND)
    foreach(PATH IN LISTS DARWIN_FILES)
        file(STRINGS "${PATH}" DIRECTIVES
            REGEX "^[ \t]*#[ \t]*include[ \t]*\"[^\"]+\"")
        foreach(DIRECTIVE IN LISTS DIRECTIVES)
            string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\".*$"
                "\\1" INCLUDE_NAME "${DIRECTIVE}")
            if(INCLUDE_NAME IN_LIST TEST_ONLY_NAMES)
                list(APPEND FOUND
                    "${PATH}: includes ${INCLUDE_NAME}, which exists only under tests/")
            endif()
        endforeach()
    endforeach()
    set(${OUT_VIOLATIONS} "${FOUND}" PARENT_SCOPE)
endfunction()

function(uml_audit_implementation_fragments ROOT OUT_VIOLATIONS)
    set(COMPOSER "${ROOT}/darwin/cpu/arm64/gadgets/program_builder.cpp")
    set(FRAGMENT_VIOLATIONS)
    file(GLOB_RECURSE FIRST_PARTY_CXX_FILES LIST_DIRECTORIES false
        "${ROOT}/darwin/*.cpp"
        "${ROOT}/darwin/*.cc"
        "${ROOT}/darwin/*.cxx"
        "${ROOT}/darwin/*.h"
        "${ROOT}/darwin/*.hh"
        "${ROOT}/darwin/*.hpp"
        "${ROOT}/darwin/*.ipp"
        "${ROOT}/darwin/*.m"
        "${ROOT}/darwin/*.mm"
        "${ROOT}/Kernarium/*.m"
        "${ROOT}/Kernarium/*.mm"
        "${ROOT}/tests/*.cpp"
        "${ROOT}/tests/*.cc"
        "${ROOT}/tests/*.cxx"
        "${ROOT}/tests/*.h"
        "${ROOT}/tests/*.hh"
        "${ROOT}/tests/*.hpp"
        "${ROOT}/tests/*.ipp"
        "${ROOT}/tests/*.m"
        "${ROOT}/tests/*.mm")
    foreach(PATH IN LISTS FIRST_PARTY_CXX_FILES)
        if(PATH STREQUAL COMPOSER)
            continue()
        endif()
        file(READ "${PATH}" CONTENT)
        if(CONTENT MATCHES
           "(^|\n)[ \t]*#[ \t]*include[ \t]+[<\"][^\">]+\\.ipp[>\"]")
            list(APPEND FRAGMENT_VIOLATIONS
                "${PATH}: only program_builder.cpp may include .ipp fragments")
        endif()
    endforeach()
    set(${OUT_VIOLATIONS} "${FRAGMENT_VIOLATIONS}" PARENT_SCOPE)
endfunction()

# Guest domains and performance runners may share process/console mechanics
# only through tests/integration/support. Importing another suite's entry
# point, or a guest launcher (scripts/boot_*.py, tools/rootfs/*/boot.py),
# couples its command-line policy and teardown behavior into an unrelated
# test.
function(uml_audit_guest_runner_imports ROOT OUT_VIOLATIONS)
    set(_violations)
    file(GLOB_RECURSE _runners LIST_DIRECTORIES false
        "${ROOT}/tests/integration/*.py"
        "${ROOT}/tests/perf/*.py")
    foreach(_path IN LISTS _runners)
        file(READ "${_path}" _content)
        if(_content MATCHES
           "(^|\n)[ \t]*(from|import)[ \t]+((tests\.)?integration\.)?(run_guest_(ltp|smp|jit|network|gpu)|boot(_[A-Za-z0-9_]+)?[ \t\r\n.,(])")
            list(APPEND _violations
                "${_path}: guest suites must share helpers through tests/integration/support")
        endif()
    endforeach()
    set(${OUT_VIOLATIONS} "${_violations}" PARENT_SCOPE)
endfunction()

set(VIOLATIONS)

file(GLOB ROOT_C_SOURCES LIST_DIRECTORIES false
    "${SOURCE_ROOT}/*.c"
    "${SOURCE_ROOT}/Makefile")
set(C_SOURCES ${ROOT_C_SOURCES})
foreach(FIRST_PARTY_SCOPE IN ITEMS
        cmake darwin dependencies kernel Kernarium scripts tests tools)
    if(NOT IS_DIRECTORY "${SOURCE_ROOT}/${FIRST_PARTY_SCOPE}")
        continue()
    endif()
    file(GLOB_RECURSE SCOPED_C_SOURCES LIST_DIRECTORIES false
        "${SOURCE_ROOT}/${FIRST_PARTY_SCOPE}/*.c"
        "${SOURCE_ROOT}/${FIRST_PARTY_SCOPE}/Makefile")
    list(APPEND C_SOURCES ${SCOPED_C_SOURCES})
endforeach()
list(REMOVE_DUPLICATES C_SOURCES)
set(ALLOWED_C_SOURCES
    "${SOURCE_ROOT}/tests/consumer/apple/main.c")
foreach(PATH IN LISTS C_SOURCES)
    if(PATH IN_LIST ALLOWED_C_SOURCES)
        # The installed-package acceptance is a genuine C consumer and is
        # not compiled into the host project.
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
    # Classify by the path inside the tree, so a checkout below a directory
    # named like a layer is not mistaken for one.
    file(RELATIVE_PATH SOURCE_PATH "${SOURCE_ROOT}" "${PATH}")
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
    if(SOURCE_PATH MATCHES "^darwin/cpu/")
        if(NORMALIZED_INCLUDE_TEXT MATCHES
           "(^|\n)(darwin/)?(runtime|device|app)/")
            list(APPEND VIOLATIONS
                "${PATH}: CPU layer includes runtime, device, or application implementation")
        endif()
    endif()
    if(SOURCE_PATH MATCHES "^darwin/memory/")
        if(NORMALIZED_INCLUDE_TEXT MATCHES
           "(^|\n)(darwin/)?(runtime|device|app)/")
            list(APPEND VIOLATIONS
                "${PATH}: memory layer includes runtime, device, or application implementation")
        endif()
    endif()
    if(SOURCE_PATH MATCHES "^darwin/device/")
        if(NORMALIZED_INCLUDE_TEXT MATCHES
           "(^|\n)(darwin/)?(runtime|app)/")
            list(APPEND VIOLATIONS
                "${PATH}: device layer includes runtime or application implementation")
        endif()
    endif()
    if(SOURCE_PATH MATCHES "^darwin/platform/" AND
       NORMALIZED_INCLUDE_TEXT MATCHES "(^|\n)(darwin/)?cpu/")
        list(APPEND VIOLATIONS
            "${PATH}: platform layer includes a CPU-owned contract")
    endif()
    # Engines do not include one another and shared CPU code includes neither.
    # The JIT owns arm64/jit/, the gadget engines own arm64/gadgets/ and
    # x86_64/, and the engine factory is the one source that composes both.
    set(FOREIGN_ENGINE_INCLUDE "")
    if(SOURCE_PATH MATCHES "^darwin/cpu/arm64/jit/")
        set(FOREIGN_ENGINE_INCLUDE "cpu/(arm64/gadgets|x86_64)/")
    elseif(SOURCE_PATH MATCHES "^darwin/cpu/(arm64/gadgets|x86_64)/")
        set(FOREIGN_ENGINE_INCLUDE "cpu/arm64/jit/")
    elseif(SOURCE_PATH MATCHES "^darwin/cpu/" AND
           NOT SOURCE_PATH MATCHES "^darwin/cpu/engine_factory\\.cpp$")
        set(FOREIGN_ENGINE_INCLUDE "cpu/(arm64/jit|arm64/gadgets|x86_64)/")
    endif()
    if(FOREIGN_ENGINE_INCLUDE AND NORMALIZED_INCLUDE_TEXT MATCHES
       "(^|\n)(darwin/)?${FOREIGN_ENGINE_INCLUDE}")
        list(APPEND VIOLATIONS
            "${PATH}: CPU source includes an engine implementation it does not own")
    endif()
    # Sandboxed iOS can execute the precompiled gadget engine without a
    # dynamic-code entitlement. Keep executable allocation and
    # publication primitives confined to the JIT arena; merely avoiding a
    # simultaneous W+X mapping is not sufficient because an RW-to-RX
    # transition is also dynamic code publication.
    if(SOURCE_PATH MATCHES "^darwin/cpu/" AND
       NOT SOURCE_PATH MATCHES "^darwin/cpu/arm64/jit/jit_arena\.cpp$" AND
       CONTENT MATCHES
       "(MAP_JIT|PROT_EXEC|VM_PROT_EXECUTE|mprotect[ \t]*\\(|mach_vm_protect[ \t]*\\(|vm_protect[ \t]*\\(|pthread_jit_write_protect|sys_icache_invalidate[ \t]*\\()")
        if(SOURCE_PATH MATCHES "^darwin/cpu/arm64/gadgets/")
            list(APPEND VIOLATIONS
                "${PATH}: non-JIT engine uses an executable-memory or code-publication primitive")
        else()
            list(APPEND VIOLATIONS
                "${PATH}: CPU executable-memory primitives must be owned by the JIT arena")
        endif()
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
    "${SOURCE_ROOT}/darwin/*.m"
    "${SOURCE_ROOT}/darwin/*.mm"
    "${SOURCE_ROOT}/Kernarium/*.m"
    "${SOURCE_ROOT}/Kernarium/*.mm"
    "${SOURCE_ROOT}/Kernarium/*.cpp"
    "${SOURCE_ROOT}/Kernarium/*.hpp"
    "${SOURCE_ROOT}/Kernarium/*.h"
    "${SOURCE_ROOT}/tests/*.cpp"
    "${SOURCE_ROOT}/tests/*.cc"
    "${SOURCE_ROOT}/tests/*.cxx"
    "${SOURCE_ROOT}/tests/*.hpp"
    "${SOURCE_ROOT}/tests/*.h"
    "${SOURCE_ROOT}/tests/*.m"
    "${SOURCE_ROOT}/tests/*.mm")
foreach(PATH IN LISTS CXX_SOURCES)
    file(READ "${PATH}" CONTENT)
    file(RELATIVE_PATH SOURCE_PATH "${SOURCE_ROOT}" "${PATH}")
    if(CONTENT MATCHES
       "(^|\\n)[ \\t]*#[ \\t]*include[ \\t]+[^\\n]*(\\.(cpp|cc|cxx|inc)|_impl\\.hpp)[\">]")
        list(APPEND VIOLATIONS
            "${PATH}: C/C++ source includes an implementation fragment")
    endif()
    if(SOURCE_PATH MATCHES "^darwin/cpu/.*\\.(h|hh|hpp|hxx)$" AND
       CONTENT MATCHES "(^|\\n)[ \\t]*namespace[ \\t]*\\{")
        list(APPEND VIOLATIONS
            "${PATH}: CPU header creates anonymous per-translation-unit ownership")
    endif()
endforeach()

uml_audit_implementation_fragments(
    "${SOURCE_ROOT}" IMPLEMENTATION_FRAGMENT_VIOLATIONS)
list(APPEND VIOLATIONS ${IMPLEMENTATION_FRAGMENT_VIOLATIONS})

uml_audit_test_only_includes("${SOURCE_ROOT}" TEST_ONLY_INCLUDE_VIOLATIONS)
list(APPEND VIOLATIONS ${TEST_ONLY_INCLUDE_VIOLATIONS})

# All first-party CTest registrations must pass through the suite helpers so
# suite labels, fixture policy, and aggregate binary dependencies cannot be
# bypassed by a raw add_test() in another directory.
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

uml_audit_guest_runner_imports("${SOURCE_ROOT}" GUEST_RUNNER_IMPORT_VIOLATIONS)
list(APPEND VIOLATIONS ${GUEST_RUNNER_IMPORT_VIOLATIONS})

# Verify that every implementation source is owned by the nearest CMakeLists.txt
# at or above it, using its complete module-relative path. A same-named source
# in another module is not evidence of ownership, and darwin/CMakeLists.txt
# composes subdirectories without owning leaf sources.
set(OWNERSHIP_GLOBS)
foreach(OWNED_TREE IN ITEMS darwin Kernarium/Runtime)
    foreach(EXTENSION IN ITEMS cpp cc cxx m mm S s)
        list(APPEND OWNERSHIP_GLOBS "${SOURCE_ROOT}/${OWNED_TREE}/*.${EXTENSION}")
    endforeach()
endforeach()
file(GLOB_RECURSE IMPLEMENTATION_SOURCES LIST_DIRECTORIES false
    ${OWNERSHIP_GLOBS})
foreach(PATH IN LISTS IMPLEMENTATION_SOURCES)
    file(RELATIVE_PATH SOURCE_RELATIVE_PATH "${SOURCE_ROOT}" "${PATH}")
    get_filename_component(MANIFEST_DIRECTORY "${PATH}" DIRECTORY)
    while(NOT EXISTS "${MANIFEST_DIRECTORY}/CMakeLists.txt" AND
          NOT MANIFEST_DIRECTORY STREQUAL SOURCE_ROOT)
        get_filename_component(MANIFEST_DIRECTORY "${MANIFEST_DIRECTORY}"
            DIRECTORY)
    endwhile()
    set(MANIFEST "${MANIFEST_DIRECTORY}/CMakeLists.txt")
    if(MANIFEST STREQUAL "${SOURCE_ROOT}/darwin/CMakeLists.txt" OR
       MANIFEST STREQUAL "${SOURCE_ROOT}/CMakeLists.txt")
        list(APPEND VIOLATIONS
            "${PATH}: implementation source has no CMake owner below the composition file")
        continue()
    endif()
    file(RELATIVE_PATH MANIFEST_RELATIVE_PATH "${MANIFEST_DIRECTORY}" "${PATH}")
    file(READ "${MANIFEST}" MANIFEST_TEXT)
    string(REGEX REPLACE "#[^\n]*" "" MANIFEST_CODE "${MANIFEST_TEXT}")
    string(FIND "${MANIFEST_CODE}" "${MANIFEST_RELATIVE_PATH}"
        RELATIVE_OWNER_INDEX)
    string(FIND "${MANIFEST_CODE}" "${SOURCE_RELATIVE_PATH}"
        SOURCE_OWNER_INDEX)
    if(RELATIVE_OWNER_INDEX EQUAL -1 AND SOURCE_OWNER_INDEX EQUAL -1)
        list(APPEND VIOLATIONS
            "${PATH}: implementation source is not owned by its nearest CMakeLists.txt")
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

if(VIOLATIONS)
    list(JOIN VIOLATIONS "\n  " FORMATTED_VIOLATIONS)
    message(FATAL_ERROR
        "Host source-tree architecture audit failed:\n  ${FORMATTED_VIOLATIONS}")
endif()
