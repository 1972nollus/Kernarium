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

# Including the audit checks the live tree; the fixtures below check that the
# implementation-fragment rule rejects an .ipp include outside the composer,
# that a darwin/ source cannot include a header that exists only under tests/,
# that a guest runner cannot import a guest launcher, and that neither a CMake
# file outside darwin/ nor an Xcode project compiles a darwin/ source.
include("${AUDIT_SCRIPT}")

file(REMOVE_RECURSE "${TEST_ROOT}")
file(WRITE "${TEST_ROOT}/darwin/cpu/arm64/gadgets/program_builder.cpp"
    "#include \"program_builder_fixture_impl.ipp\"\n")
file(WRITE "${TEST_ROOT}/darwin/cpu/arm64/gadgets/program_builder_fixture_impl.ipp"
    "// fixture\n")
uml_audit_implementation_fragments("${TEST_ROOT}" COMPOSER_VIOLATIONS)
if(COMPOSER_VIOLATIONS)
    message(FATAL_ERROR
        "source audit rejected the composer's fragment: ${COMPOSER_VIOLATIONS}")
endif()

file(WRITE "${TEST_ROOT}/tests/unit/arm64/fixture_tests.cpp"
    "#include \"program_builder_fixture_impl.ipp\"\n")
uml_audit_implementation_fragments("${TEST_ROOT}" FOREIGN_VIOLATIONS)
if(NOT FOREIGN_VIOLATIONS MATCHES "fixture_tests.cpp: only program_builder.cpp")
    message(FATAL_ERROR "source audit accepted an .ipp include outside the composer")
endif()

file(WRITE "${TEST_ROOT}/darwin/device/fixture/fixture_device.cpp"
    "#include \"fixture_hook_testing.hpp\"\n")
file(WRITE "${TEST_ROOT}/tests/unit/fixture/fixture_hook_testing.hpp"
    "// fixture\n")
uml_audit_test_only_includes("${TEST_ROOT}" TEST_ONLY_VIOLATIONS)
if(NOT TEST_ONLY_VIOLATIONS MATCHES
   "fixture_device.cpp: includes fixture_hook_testing.hpp, which exists only under tests/")
    message(FATAL_ERROR "source audit accepted a darwin/ include of a test-only header")
endif()
file(RENAME "${TEST_ROOT}/tests/unit/fixture/fixture_hook_testing.hpp"
    "${TEST_ROOT}/darwin/device/fixture/fixture_hook_testing.hpp")
uml_audit_test_only_includes("${TEST_ROOT}" BESIDE_SOURCE_VIOLATIONS)
if(BESIDE_SOURCE_VIOLATIONS)
    message(FATAL_ERROR
        "source audit rejected a hook header beside its source: ${BESIDE_SOURCE_VIOLATIONS}")
endif()

file(WRITE "${TEST_ROOT}/tests/integration/fixture/run_fixture.py"
    "import sys\nimport boot_kde_vnc  # noqa: E402\n")
uml_audit_guest_runner_imports("${TEST_ROOT}" LAUNCHER_IMPORT_VIOLATIONS)
if(NOT LAUNCHER_IMPORT_VIOLATIONS MATCHES "run_fixture.py: guest suites must share")
    message(FATAL_ERROR "source audit accepted a guest runner importing a launcher")
endif()
file(WRITE "${TEST_ROOT}/tests/integration/fixture/run_fixture.py"
    "import sys\nimport kde_session  # noqa: E402\nimport bootstrap_fixture\n")
uml_audit_guest_runner_imports("${TEST_ROOT}" SUPPORT_IMPORT_VIOLATIONS)
if(SUPPORT_IMPORT_VIOLATIONS)
    message(FATAL_ERROR
        "source audit rejected a guest runner's support import: ${SUPPORT_IMPORT_VIOLATIONS}")
endif()

# The live audit reads this file too, so the fixtures spell the darwin
# directory through a variable rather than as a literal source path.
set(FIXTURE_TREE darwin)
file(WRITE "${TEST_ROOT}/tests/unit/fixture/CMakeLists.txt"
    "add_executable(fixture_tests fixture_tests.cpp\n"
    "    \${PROJECT_SOURCE_DIR}/${FIXTURE_TREE}/device/fixture/fixture_device.cpp)\n")
file(WRITE "${TEST_ROOT}/Kernarium/Fixture.xcodeproj/project.pbxproj"
    "\t\tD1 /* fixture.mm */ = {isa = PBXFileReference; name = fixture.mm; "
    "path = ../${FIXTURE_TREE}/platform/apple/fixture.mm; sourceTree = SOURCE_ROOT; };\n")
uml_audit_foreign_darwin_sources("${TEST_ROOT}" FOREIGN_SOURCE_VIOLATIONS)
if(NOT FOREIGN_SOURCE_VIOLATIONS MATCHES
   "fixture/CMakeLists.txt: compiles ${FIXTURE_TREE}/device/fixture/fixture_device\\.cpp")
    message(FATAL_ERROR "source audit accepted a test CMake file compiling a darwin/ source")
endif()
if(NOT FOREIGN_SOURCE_VIOLATIONS MATCHES
   "project.pbxproj: an Xcode target compiles \\.\\./${FIXTURE_TREE}/platform/apple/fixture\\.mm")
    message(FATAL_ERROR "source audit accepted an Xcode target compiling a darwin/ source")
endif()
file(WRITE "${TEST_ROOT}/tests/unit/fixture/CMakeLists.txt"
    "add_executable(fixture_tests fixture_tests.cpp)\n"
    "target_include_directories(fixture_tests PRIVATE\n"
    "    \${PROJECT_SOURCE_DIR}/${FIXTURE_TREE}/device/fixture)\n"
    "target_link_libraries(fixture_tests PRIVATE UML::FixtureDevice)\n")
file(WRITE "${TEST_ROOT}/Kernarium/Fixture.xcodeproj/project.pbxproj"
    "\t\tD2 /* Fixture.xcframework */ = {isa = PBXFileReference; "
    "name = Fixture.xcframework; "
    "path = \"../.build/kernarium-xcframeworks/Fixture.xcframework\"; "
    "sourceTree = SOURCE_ROOT; };\n")
uml_audit_foreign_darwin_sources("${TEST_ROOT}" OWNER_PRODUCT_VIOLATIONS)
if(OWNER_PRODUCT_VIOLATIONS)
    message(FATAL_ERROR
        "source audit rejected targets linking the owner's product: ${OWNER_PRODUCT_VIOLATIONS}")
endif()

# The layer, engine-ownership, executable-memory, implementation-include,
# CPU-header and test-registration rules read the whole tree, so the audit
# itself runs over a fixture tree that breaks each rule beside a file the rule
# accepts. Every fixture starts on its second line, so a rule cannot pass by
# matching only at the start of a file.
file(REMOVE_RECURSE "${TEST_ROOT}")
file(WRITE "${TEST_ROOT}/CMakeLists.txt" "")
file(WRITE "${TEST_ROOT}/docs/architecture.md" "")
function(uml_write_audit_fixture PATH TEXT)
    file(WRITE "${TEST_ROOT}/${PATH}" "// fixture\n${TEXT}\n")
endfunction()
set(REJECTED)
set(ACCEPTED)

uml_write_audit_fixture(darwin/cpu/fixture/cpu_layer.cpp
    "#include \"runtime/fixture.hpp\"")
uml_write_audit_fixture(darwin/memory/fixture/memory_layer.cpp
    "#include \"device/fixture.hpp\"")
uml_write_audit_fixture(darwin/device/fixture/device_layer.cpp
    "#include <app/fixture.hpp>")
uml_write_audit_fixture(darwin/platform/fixture/platform_layer.hpp
    "#include \"darwin/cpu/fixture.hpp\"")
uml_write_audit_fixture(darwin/device/fixture/device_memory.cpp
    "#include \"memory/fixture.hpp\"")
uml_write_audit_fixture(darwin/runtime/fixture/runtime_device.cpp
    "#include \"device/fixture.hpp\"")
list(APPEND REJECTED
    "cpu/fixture/cpu_layer\\.cpp: [A-Za-z]+ layer includes"
    "memory/fixture/memory_layer\\.cpp: [A-Za-z]+ layer includes"
    "device/fixture/device_layer\\.cpp: [A-Za-z]+ layer includes"
    "platform/fixture/platform_layer\\.hpp: [A-Za-z]+ layer includes")
uml_write_audit_fixture(darwin/memory/fixture/tab_layer.cpp
    "#include\t\"runtime/fixture.hpp\"")
list(APPEND REJECTED "tab_layer\\.cpp: [A-Za-z]+ layer includes")
list(APPEND ACCEPTED
    "device_memory\\.cpp: [A-Za-z]+ layer"
    "runtime_device\\.cpp: [A-Za-z]+ layer")

uml_write_audit_fixture(darwin/cpu/fixture/fragment_source.cpp
    "#include \"fixture_table.cpp\"")
uml_write_audit_fixture(tests/fixture/fragment_contract.cpp
    "#include \"fixture_contract.inc\"")
uml_write_audit_fixture(Kernarium/Runtime/fragment_impl.mm
    "#include \"runtime_impl.hpp\"")
uml_write_audit_fixture(darwin/cpu/fixture/header_include.cpp
    "#include \"fixture_contract.hpp\"")
list(APPEND REJECTED
    "fragment_source\\.cpp: C/C\\+\\+ source includes an implementation fragment"
    "fragment_contract\\.cpp: C/C\\+\\+ source includes an implementation fragment"
    "fragment_impl\\.mm: C/C\\+\\+ source includes an implementation fragment")
list(APPEND ACCEPTED "header_include\\.cpp: C/C\\+\\+ source")

uml_write_audit_fixture(darwin/cpu/fixture/anonymous.hpp
    "namespace fixture {\nnamespace {\n}\n}")
uml_write_audit_fixture(darwin/cpu/fixture/anonymous_source.cpp
    "namespace {\n}")
uml_write_audit_fixture(darwin/device/fixture/anonymous_device.hpp
    "namespace {\n}")
list(APPEND REJECTED
    "anonymous\\.hpp: CPU header creates anonymous per-translation-unit ownership")
list(APPEND ACCEPTED
    "anonymous_source\\.cpp: CPU header"
    "anonymous_device\\.hpp: CPU header")

uml_write_audit_fixture(darwin/cpu/arm64/jit/jit_fixture.cpp
    "#include \"cpu/arm64/gadgets/fixture.hpp\"")
uml_write_audit_fixture(darwin/cpu/x86_64/x86_fixture.cpp
    "#include \"cpu/arm64/jit/fixture.hpp\"")
uml_write_audit_fixture(darwin/cpu/shared_fixture.cpp
    "#include \"cpu/x86_64/fixture.hpp\"")
uml_write_audit_fixture(darwin/cpu/engine_factory.cpp
    "#include \"cpu/arm64/jit/fixture.hpp\"\n#include \"cpu/x86_64/fixture.hpp\"")
list(APPEND REJECTED
    "jit_fixture\\.cpp: CPU source includes an engine implementation"
    "x86_fixture\\.cpp: CPU source includes an engine implementation"
    "shared_fixture\\.cpp: CPU source includes an engine implementation")
list(APPEND ACCEPTED "engine_factory\\.cpp: CPU source includes")

uml_write_audit_fixture(darwin/cpu/arm64/gadgets/gadget_exec.cpp
    "int publish() { return mprotect(nullptr, 0, 0); }")
uml_write_audit_fixture(darwin/cpu/fixture/cpu_exec.cpp
    "int protection = PROT_EXEC;")
uml_write_audit_fixture(darwin/cpu/arm64/jit/jit_arena.cpp
    "int publish() { return mprotect(nullptr, 0, 0); }")
uml_write_audit_fixture(darwin/memory/fixture/memory_exec.cpp
    "int protection = PROT_EXEC;")
list(APPEND REJECTED
    "gadget_exec\\.cpp: [^\n]*executable-memory"
    "cpu_exec\\.cpp: [^\n]*executable-memory")
list(APPEND ACCEPTED
    "jit_arena\\.cpp: [^\n]*executable-memory"
    "memory_exec\\.cpp: [^\n]*executable-memory")

uml_write_audit_fixture(tests/fixture/CMakeLists.txt
    "add_test(NAME fixture COMMAND fixture)")
uml_write_audit_fixture(tests/fixture/Suite.cmake "ADD_TEST (fixture fixture)")
# CMake reads a carriage return as a space, before the command and before its
# parenthesis alike.
uml_write_audit_fixture(tests/fixture/Carriage.cmake
    "\t\r add_test \r(NAME fixture COMMAND fixture)")
uml_write_audit_fixture(tests/fixture/Commented.cmake
    "# add_test(NAME fixture COMMAND fixture)")
uml_write_audit_fixture(cmake/UmlTesting.cmake
    "add_test(NAME fixture COMMAND fixture)")
# tools/ holds guest-side CMake projects that never join the host build.
uml_write_audit_fixture(tools/fixture/CMakeLists.txt
    "add_test(NAME fixture COMMAND fixture)")
list(APPEND REJECTED
    "fixture/CMakeLists\\.txt: raw add_test bypasses"
    "fixture/Suite\\.cmake: raw add_test bypasses"
    "fixture/Carriage\\.cmake: raw add_test bypasses")
list(APPEND ACCEPTED
    "Commented\\.cmake: raw add_test"
    "UmlTesting\\.cmake: raw add_test"
    "tools/fixture/CMakeLists\\.txt: raw add_test")

# A darwin/ owner names its own sources.
uml_write_audit_fixture(darwin/fixture/CMakeLists.txt
    "add_library(fixture \${PROJECT_SOURCE_DIR}/${FIXTURE_TREE}/fixture/owned.cpp)")
list(APPEND ACCEPTED "darwin/fixture/CMakeLists\\.txt: compiles")

execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${TEST_ROOT}" -P "${AUDIT_SCRIPT}"
    RESULT_VARIABLE AUDIT_RESULT
    OUTPUT_VARIABLE AUDIT_OUTPUT
    ERROR_VARIABLE AUDIT_OUTPUT)
if(AUDIT_RESULT EQUAL 0)
    message(FATAL_ERROR "source audit accepted the fixture tree")
endif()
foreach(EXPECTED IN LISTS REJECTED)
    if(NOT AUDIT_OUTPUT MATCHES "/${EXPECTED}")
        message(FATAL_ERROR
            "source audit did not report ${EXPECTED}:\n${AUDIT_OUTPUT}")
    endif()
endforeach()
foreach(UNEXPECTED IN LISTS ACCEPTED)
    if(AUDIT_OUTPUT MATCHES "/${UNEXPECTED}")
        message(FATAL_ERROR
            "source audit reported ${UNEXPECTED}:\n${AUDIT_OUTPUT}")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
