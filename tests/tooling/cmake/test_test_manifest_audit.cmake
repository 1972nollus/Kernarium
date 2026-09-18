# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED AUDIT_SCRIPT OR AUDIT_SCRIPT STREQUAL "")
    message(FATAL_ERROR "AUDIT_SCRIPT is required")
endif()
if(NOT DEFINED TEST_ROOT OR TEST_ROOT STREQUAL "")
    message(FATAL_ERROR "TEST_ROOT is required")
endif()
if(NOT DEFINED REGISTRATION_POLICY_MODULE OR
   REGISTRATION_POLICY_MODULE STREQUAL "")
    message(FATAL_ERROR "REGISTRATION_POLICY_MODULE is required")
endif()
if(NOT DEFINED UML_TESTING_MODULE OR UML_TESTING_MODULE STREQUAL "")
    message(FATAL_ERROR "UML_TESTING_MODULE is required")
endif()
get_filename_component(TEST_ROOT_NAME "${TEST_ROOT}" NAME)
if(NOT TEST_ROOT_NAME MATCHES "^test-manifest-audit-fixture")
    message(FATAL_ERROR
        "Refusing to replace an unexpected test-manifest fixture: ${TEST_ROOT}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/build" "${TEST_ROOT}/tests/tooling")
file(WRITE "${TEST_ROOT}/tests/tooling/owner.txt" "fixture owner\n")

string(CONCAT MANIFEST_CONTENT
    "name\towner\tsuite\tavailability\n"
    "suite-label-sample\ttests/tooling/owner.txt\ttooling\tretained\n")
file(WRITE "${TEST_ROOT}/tests/test-manifest.tsv" "${MANIFEST_CONTENT}")

function(_uml_write_ctest_fixture LABEL_DECLARATION)
    file(WRITE "${TEST_ROOT}/build/CTestTestfile.cmake"
        "add_test([=[suite-label-sample]=] [=[${CMAKE_COMMAND}]=] -E true)\n"
        "${LABEL_DECLARATION}")
endfunction()

function(_uml_run_manifest_audit EXPECT_SUCCESS EXPECTED_DIAGNOSTIC)
    set(REQUIRED_CONDITIONAL_TESTS "")
    if(ARGC GREATER 2)
        set(REQUIRED_CONDITIONAL_TESTS "${ARGV2}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_ROOT=${TEST_ROOT}/build"
            "-DMANIFEST=${TEST_ROOT}/tests/test-manifest.tsv"
            -DEXPECTED_SUITES=tooling
            "-DREQUIRED_CONDITIONAL_TESTS=${REQUIRED_CONDITIONAL_TESTS}"
            -P "${AUDIT_SCRIPT}"
        RESULT_VARIABLE RESULT
        OUTPUT_VARIABLE OUTPUT
        ERROR_VARIABLE ERROR)
    set(COMBINED_OUTPUT "${OUTPUT}\n${ERROR}")
    if(EXPECT_SUCCESS)
        if(NOT RESULT EQUAL 0)
            message(FATAL_ERROR
                "Valid test-manifest fixture was rejected:\n${COMBINED_OUTPUT}")
        endif()
    elseif(RESULT EQUAL 0)
        message(FATAL_ERROR
            "Invalid test-manifest fixture was accepted: "
            "${EXPECTED_DIAGNOSTIC}")
    elseif(NOT COMBINED_OUTPUT MATCHES "${EXPECTED_DIAGNOSTIC}")
        message(FATAL_ERROR
            "Invalid fixture failed for the wrong reason; expected "
            "'${EXPECTED_DIAGNOSTIC}':\n${COMBINED_OUTPUT}")
    endif()
endfunction()

_uml_write_ctest_fixture(
    "set_tests_properties([=[suite-label-sample]=] PROPERTIES LABELS [=[tooling]=])\n")
_uml_run_manifest_audit(TRUE "")

string(REPLACE
    "suite-label-sample\ttests/tooling/owner.txt\ttooling\tretained"
    "suite-label-sample\ttests/tooling/owner.txt\ttooling\tconditional"
    CONDITIONAL_MANIFEST_CONTENT "${MANIFEST_CONTENT}")
file(WRITE "${TEST_ROOT}/tests/test-manifest.tsv"
    "${CONDITIONAL_MANIFEST_CONTENT}")
_uml_run_manifest_audit(FALSE
    "Registered conditional test is missing from REQUIRED_CONDITIONAL_TESTS")
_uml_run_manifest_audit(TRUE "" suite-label-sample)

# Requiring a conditional row must make deletion of its active CTest
# registration observable even while the manifest entry remains intact.
file(WRITE "${TEST_ROOT}/build/CTestTestfile.cmake" "")
_uml_run_manifest_audit(FALSE
    "Required tooling test is absent from the generated CTest registry"
    suite-label-sample)
file(WRITE "${TEST_ROOT}/tests/test-manifest.tsv" "${MANIFEST_CONTENT}")
_uml_write_ctest_fixture(
    "set_tests_properties([=[suite-label-sample]=] PROPERTIES LABELS [=[tooling]=])\n")

string(REPLACE
    "suite-label-sample\ttests/tooling/owner.txt\ttooling\tretained"
    "suite-label-sample\ttests/tooling/owner.txt\tunit\tretained"
    PHYSICAL_SUITE_MISMATCH "${MANIFEST_CONTENT}")
file(WRITE "${TEST_ROOT}/tests/test-manifest.tsv"
    "${PHYSICAL_SUITE_MISMATCH}")
_uml_run_manifest_audit(FALSE
    "Test owner is outside tests/unit for unit suite")

# A benchmark runner may remain a manual tests/perf asset. Once a CTest row
# claims it as an owner, however, that executable test must move to the suite's
# physical tree rather than disguising a tooling test as a benchmark campaign.
file(MAKE_DIRECTORY "${TEST_ROOT}/tests/perf")
file(WRITE "${TEST_ROOT}/tests/perf/run_guest_bench.py" "# fixture\n")
string(REPLACE
    "suite-label-sample\ttests/tooling/owner.txt\ttooling\tretained"
    "suite-label-sample\ttests/perf/run_guest_bench.py\ttooling\tretained"
    NONCANONICAL_OWNER "${MANIFEST_CONTENT}")
file(WRITE "${TEST_ROOT}/tests/test-manifest.tsv" "${NONCANONICAL_OWNER}")
_uml_run_manifest_audit(FALSE
    "Test owner is outside tests/tooling for tooling suite")
file(WRITE "${TEST_ROOT}/tests/test-manifest.tsv" "${MANIFEST_CONTENT}")

_uml_write_ctest_fixture(
    "set_tests_properties([=[suite-label-sample]=] PROPERTIES LABELS [=[unit]=])\n")
_uml_run_manifest_audit(FALSE "suite label differs")

_uml_write_ctest_fixture("")
_uml_run_manifest_audit(FALSE "has no suite label")

_uml_write_ctest_fixture(
    "set_tests_properties([=[suite-label-sample]=] PROPERTIES LABELS [=[tooling;unit]=])\n")
_uml_run_manifest_audit(FALSE "exactly one suite label")

include("${REGISTRATION_POLICY_MODULE}")
set(REGISTRATION_FIXTURE "${TEST_ROOT}/registration-source")
file(MAKE_DIRECTORY "${REGISTRATION_FIXTURE}/tests")
file(WRITE "${REGISTRATION_FIXTURE}/CMakeLists.txt"
    "uml_add_unit_test(NAME sample COMMAND true)\n")
uml_find_raw_test_registrations(
    SOURCE_ROOT "${REGISTRATION_FIXTURE}"
    OUT_VIOLATIONS REGISTRATION_VIOLATIONS)
if(REGISTRATION_VIOLATIONS)
    message(FATAL_ERROR
        "Centralized registration fixture was rejected: ${REGISTRATION_VIOLATIONS}")
endif()
foreach(RAW_COMMAND IN ITEMS add_test ADD_TEST Add_Test)
    file(WRITE "${REGISTRATION_FIXTURE}/tests/CMakeLists.txt"
        "${RAW_COMMAND}(NAME sample COMMAND true)\n")
    uml_find_raw_test_registrations(
        SOURCE_ROOT "${REGISTRATION_FIXTURE}"
        OUT_VIOLATIONS REGISTRATION_VIOLATIONS)
    list(LENGTH REGISTRATION_VIOLATIONS REGISTRATION_VIOLATION_COUNT)
    if(NOT REGISTRATION_VIOLATION_COUNT EQUAL 1)
        message(FATAL_ERROR
            "Mixed-case raw registration was not rejected exactly once: "
            "${RAW_COMMAND}: ${REGISTRATION_VIOLATIONS}")
    endif()
endforeach()

function(_uml_write_configure_fixture RAW_REGISTRATION)
    string(CONCAT _content
        "cmake_minimum_required(VERSION 3.27)\n"
        "project(test_registry_fixture LANGUAGES NONE)\n"
        "enable_testing()\n"
        "include(\"${UML_TESTING_MODULE}\")\n"
        "uml_add_tooling_test(NAME suite-label-sample "
        "COMMAND \"${TEST_ROOT}/tests/tooling/owner.txt\" LABELS audit)\n")
    if(RAW_REGISTRATION)
        string(APPEND _content
            "cmake_language(CALL ADD_TEST NAME raw-bypass "
            "COMMAND \"${TEST_ROOT}/tests/tooling/owner.txt\")\n")
    endif()
    string(APPEND _content "uml_validate_registered_test_manifest()\n")
    file(WRITE "${TEST_ROOT}/CMakeLists.txt" "${_content}")
endfunction()

_uml_write_configure_fixture(FALSE)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${TEST_ROOT}"
        -B "${TEST_ROOT}/configure-valid"
    RESULT_VARIABLE CONFIGURE_RESULT
    OUTPUT_VARIABLE CONFIGURE_OUTPUT
    ERROR_VARIABLE CONFIGURE_ERROR)
if(NOT CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Centralized configure fixture failed:\n${CONFIGURE_OUTPUT}\n${CONFIGURE_ERROR}")
endif()

_uml_write_configure_fixture(TRUE)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${TEST_ROOT}"
        -B "${TEST_ROOT}/configure-raw-bypass"
    RESULT_VARIABLE CONFIGURE_RESULT
    OUTPUT_VARIABLE CONFIGURE_OUTPUT
    ERROR_VARIABLE CONFIGURE_ERROR)
set(CONFIGURE_COMBINED "${CONFIGURE_OUTPUT}\n${CONFIGURE_ERROR}")
if(CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Indirect raw CTest registration bypassed the registry")
endif()
if(NOT CONFIGURE_COMBINED MATCHES
   "CTest registrations differ from the centralized suite registry")
    message(FATAL_ERROR
        "Raw configure fixture failed for the wrong reason:\n${CONFIGURE_COMBINED}")
endif()
