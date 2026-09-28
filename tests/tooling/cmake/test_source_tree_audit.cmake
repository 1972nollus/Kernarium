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

file(REMOVE_RECURSE "${TEST_ROOT}")
