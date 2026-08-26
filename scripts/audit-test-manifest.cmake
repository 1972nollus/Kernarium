# SPDX-License-Identifier: GPL-2.0-or-later
#
# Compare the generated CTest registry with the checked-in test ownership
# manifest.  A unit-only or integration-only configure checks its enabled
# suite; a combined configure checks both directions completely.

if(NOT DEFINED BUILD_ROOT)
    message(FATAL_ERROR "BUILD_ROOT is required")
endif()
if(NOT DEFINED MANIFEST)
    message(FATAL_ERROR "MANIFEST is required")
endif()
if(NOT EXISTS "${MANIFEST}")
    message(FATAL_ERROR "Test manifest is missing: ${MANIFEST}")
endif()
get_filename_component(MANIFEST_DIRECTORY "${MANIFEST}" DIRECTORY)
get_filename_component(SOURCE_ROOT "${MANIFEST_DIRECTORY}" DIRECTORY)

if(NOT DEFINED EXPECTED_SUITES)
    set(EXPECTED_SUITES "unit,integration")
endif()
string(REPLACE "," ";" EXPECTED_SUITES "${EXPECTED_SUITES}")
if(NOT DEFINED REQUIRED_CONDITIONAL_TESTS)
    set(REQUIRED_CONDITIONAL_TESTS "")
endif()
string(REPLACE "," ";" REQUIRED_CONDITIONAL_TESTS
    "${REQUIRED_CONDITIONAL_TESTS}")

find_program(CTEST_EXECUTABLE NAMES ctest REQUIRED)
execute_process(
    COMMAND "${CTEST_EXECUTABLE}" --test-dir "${BUILD_ROOT}" -N -V
    RESULT_VARIABLE CTEST_RESULT
    OUTPUT_VARIABLE CTEST_OUTPUT
    ERROR_VARIABLE CTEST_ERROR)
if(NOT CTEST_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Unable to enumerate CTest registrations in ${BUILD_ROOT}:\n${CTEST_ERROR}")
endif()

file(STRINGS "${MANIFEST}" MANIFEST_ROWS)
list(LENGTH MANIFEST_ROWS MANIFEST_ROW_COUNT)
if(MANIFEST_ROW_COUNT LESS 2)
    message(FATAL_ERROR "Test manifest has no data rows: ${MANIFEST}")
endif()
list(GET MANIFEST_ROWS 0 MANIFEST_HEADER)
if(NOT MANIFEST_HEADER STREQUAL
       "name\towner\tsuite\tavailability")
    message(FATAL_ERROR
        "Test manifest has an unsupported header: ${MANIFEST_HEADER}")
endif()

set(MANIFEST_NAMES)
set(MANIFEST_OWNERS)
set(MANIFEST_SUITES)
set(MANIFEST_AVAILABILITIES)
foreach(ROW IN LISTS MANIFEST_ROWS)
    if(ROW MATCHES "^#" OR ROW STREQUAL "" OR
       ROW STREQUAL "${MANIFEST_HEADER}")
        continue()
    endif()
    string(REPLACE "\t" ";" FIELDS "${ROW}")
    list(LENGTH FIELDS FIELD_COUNT)
    if(NOT FIELD_COUNT EQUAL 4)
        message(FATAL_ERROR "Malformed test manifest row: ${ROW}")
    endif()
    list(GET FIELDS 0 CURRENT_NAME)
    list(GET FIELDS 1 OWNER)
    list(GET FIELDS 2 SUITE)
    list(GET FIELDS 3 AVAILABILITY)
    if(CURRENT_NAME STREQUAL "" OR CURRENT_NAME STREQUAL "-")
        message(FATAL_ERROR "Test manifest has no current test name: ${ROW}")
    endif()
    if(OWNER STREQUAL "" OR OWNER STREQUAL "-" OR
       IS_ABSOLUTE "${OWNER}" OR OWNER MATCHES "(^|/)\\.\\.(/|$)" OR
       NOT EXISTS "${SOURCE_ROOT}/${OWNER}")
        message(FATAL_ERROR "Test manifest has an invalid owner: ${ROW}")
    endif()
    if(NOT SUITE MATCHES "^(unit|tooling|integration)$")
        message(FATAL_ERROR "Test manifest has an invalid suite: ${ROW}")
    endif()
    set(EXPECTED_OWNER_ROOT "tests/${SUITE}")
    if(NOT OWNER STREQUAL "${EXPECTED_OWNER_ROOT}" AND
       NOT OWNER MATCHES "^${EXPECTED_OWNER_ROOT}/")
        message(FATAL_ERROR
            "Test owner is outside ${EXPECTED_OWNER_ROOT} for ${SUITE} "
            "suite: ${ROW}")
    endif()
    if(NOT AVAILABILITY MATCHES "^(retained|conditional)$")
        message(FATAL_ERROR "Test manifest has invalid availability: ${ROW}")
    endif()
    list(FIND MANIFEST_NAMES "${CURRENT_NAME}" DUPLICATE_INDEX)
    if(NOT DUPLICATE_INDEX EQUAL -1)
        message(FATAL_ERROR "Duplicate current test manifest name: ${CURRENT_NAME}")
    endif()
    list(APPEND MANIFEST_NAMES "${CURRENT_NAME}")
    list(APPEND MANIFEST_OWNERS "${OWNER}")
    list(APPEND MANIFEST_SUITES "${SUITE}")
    list(APPEND MANIFEST_AVAILABILITIES "${AVAILABILITY}")
endforeach()

# A conditional row is registered only in the configurations explicitly named
# by the owning CMake file. Validate those declarations before comparing the
# generated registry, which prevents a typo from silently weakening coverage.
foreach(CONDITIONAL_NAME IN LISTS REQUIRED_CONDITIONAL_TESTS)
    if(CONDITIONAL_NAME STREQUAL "")
        continue()
    endif()
    list(FIND MANIFEST_NAMES "${CONDITIONAL_NAME}" CONDITIONAL_INDEX)
    if(CONDITIONAL_INDEX EQUAL -1)
        message(FATAL_ERROR
            "Required conditional test is absent from ${MANIFEST}: ${CONDITIONAL_NAME}")
    endif()
    list(GET MANIFEST_AVAILABILITIES ${CONDITIONAL_INDEX}
        CONDITIONAL_AVAILABILITY)
    if(NOT CONDITIONAL_AVAILABILITY STREQUAL "conditional")
        message(FATAL_ERROR
            "Required conditional test is not conditional in ${MANIFEST}: "
            "${CONDITIONAL_NAME}")
    endif()
    list(GET MANIFEST_SUITES ${CONDITIONAL_INDEX} CONDITIONAL_SUITE)
    list(FIND EXPECTED_SUITES "${CONDITIONAL_SUITE}" CONDITIONAL_SUITE_ENABLED)
    if(CONDITIONAL_SUITE_ENABLED EQUAL -1)
        message(FATAL_ERROR
            "Required conditional test belongs to a disabled suite: "
            "${CONDITIONAL_NAME} (${CONDITIONAL_SUITE})")
    endif()
endforeach()

string(REPLACE "\r\n" "\n" CTEST_OUTPUT "${CTEST_OUTPUT}")
string(REGEX MATCHALL "Test[ \t]+#[0-9]+:[ \t]*[^\n]+" CTEST_ROWS "${CTEST_OUTPUT}")
string(REGEX MATCHALL
    "Labels:[^\n]*\n[ \t]*Test[ \t]+#[0-9]+:[ \t]*[^\n]+"
    CTEST_LABELED_ROWS "${CTEST_OUTPUT}")
set(CTEST_LABELED_NAMES)
set(CTEST_LABEL_SETS)
foreach(ROW IN LISTS CTEST_LABELED_ROWS)
    string(REGEX REPLACE
        "^Labels:[ \t]*([^\n]*)\n[ \t]*Test[ \t]+#[0-9]+:[ \t]*(.*)$"
        "\\1" TEST_LABELS "${ROW}")
    string(REGEX REPLACE
        "^Labels:[^\n]*\n[ \t]*Test[ \t]+#[0-9]+:[ \t]*(.*)$"
        "\\1" LABELED_TEST_NAME "${ROW}")
    list(FIND CTEST_LABELED_NAMES "${LABELED_TEST_NAME}" DUPLICATE_INDEX)
    if(NOT DUPLICATE_INDEX EQUAL -1)
        message(FATAL_ERROR
            "Duplicate CTest label record: ${LABELED_TEST_NAME}")
    endif()
    list(APPEND CTEST_LABELED_NAMES "${LABELED_TEST_NAME}")
    list(APPEND CTEST_LABEL_SETS "${TEST_LABELS}")
endforeach()

set(CTEST_NAMES)
foreach(ROW IN LISTS CTEST_ROWS)
    string(REGEX REPLACE "^[ \t]*Test[ \t]+#[0-9]+:[ \t]*" "" TEST_NAME "${ROW}")
    if(TEST_NAME STREQUAL "")
        message(FATAL_ERROR "Unable to parse CTest row: ${ROW}")
    endif()
    list(FIND CTEST_NAMES "${TEST_NAME}" DUPLICATE_INDEX)
    if(NOT DUPLICATE_INDEX EQUAL -1)
        message(FATAL_ERROR "Duplicate CTest registration: ${TEST_NAME}")
    endif()
    list(APPEND CTEST_NAMES "${TEST_NAME}")
    list(FIND MANIFEST_NAMES "${TEST_NAME}" MANIFEST_INDEX)
    if(MANIFEST_INDEX EQUAL -1)
        message(FATAL_ERROR
            "CTest registration is missing from ${MANIFEST}: ${TEST_NAME}")
    endif()
    list(GET MANIFEST_AVAILABILITIES ${MANIFEST_INDEX} AVAILABILITY)
    if(NOT AVAILABILITY STREQUAL "retained" AND
       NOT AVAILABILITY STREQUAL "conditional")
        message(FATAL_ERROR
            "Registered test is not retained in ${MANIFEST}: ${TEST_NAME}")
    endif()
    if(AVAILABILITY STREQUAL "conditional")
        list(FIND REQUIRED_CONDITIONAL_TESTS "${TEST_NAME}"
            CONDITIONAL_REQUIRED_INDEX)
        if(CONDITIONAL_REQUIRED_INDEX EQUAL -1)
            message(FATAL_ERROR
                "Registered conditional test is missing from "
                "REQUIRED_CONDITIONAL_TESTS: ${TEST_NAME}")
        endif()
    endif()

    # Inspect the generated CTest registry, not only helper bookkeeping. This
    # catches a raw or post-registration mutation that omits the suite label or
    # assigns a test to a different aggregate than its manifest owner.
    list(FIND CTEST_LABELED_NAMES "${TEST_NAME}" LABELED_INDEX)
    if(LABELED_INDEX EQUAL -1)
        message(FATAL_ERROR
            "CTest registration has no suite label: ${TEST_NAME}")
    endif()
    list(GET CTEST_LABEL_SETS ${LABELED_INDEX} TEST_LABELS)
    string(REGEX MATCHALL "[^ \t]+" TEST_LABEL_LIST "${TEST_LABELS}")
    set(TEST_SUITE_LABELS)
    foreach(LABEL IN LISTS TEST_LABEL_LIST)
        if(LABEL MATCHES "^(unit|tooling|integration)$")
            list(APPEND TEST_SUITE_LABELS "${LABEL}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES TEST_SUITE_LABELS)
    list(LENGTH TEST_SUITE_LABELS TEST_SUITE_LABEL_COUNT)
    if(NOT TEST_SUITE_LABEL_COUNT EQUAL 1)
        message(FATAL_ERROR
            "CTest registration must have exactly one suite label: "
            "${TEST_NAME} (${TEST_LABELS})")
    endif()
    list(GET TEST_SUITE_LABELS 0 TEST_SUITE_LABEL)
    list(GET MANIFEST_SUITES ${MANIFEST_INDEX} MANIFEST_SUITE)
    if(NOT TEST_SUITE_LABEL STREQUAL MANIFEST_SUITE)
        message(FATAL_ERROR
            "CTest suite label differs from ${MANIFEST}: ${TEST_NAME} "
            "(${TEST_SUITE_LABEL} versus ${MANIFEST_SUITE})")
    endif()
    list(FIND EXPECTED_SUITES "${TEST_SUITE_LABEL}" SUITE_ENABLED)
    if(SUITE_ENABLED EQUAL -1)
        message(FATAL_ERROR
            "CTest registered a disabled suite: ${TEST_NAME} "
            "(${TEST_SUITE_LABEL})")
    endif()
endforeach()

list(LENGTH MANIFEST_NAMES MANIFEST_NAME_COUNT)
if(MANIFEST_NAME_COUNT GREATER 0)
    math(EXPR MANIFEST_LAST_INDEX "${MANIFEST_NAME_COUNT} - 1")
else()
    set(MANIFEST_LAST_INDEX -1)
endif()
foreach(MANIFEST_INDEX RANGE 0 ${MANIFEST_LAST_INDEX})
    if(MANIFEST_INDEX LESS 0)
        break()
    endif()
    list(GET MANIFEST_SUITES ${MANIFEST_INDEX} SUITE)
    list(GET MANIFEST_AVAILABILITIES ${MANIFEST_INDEX} AVAILABILITY)
    list(FIND EXPECTED_SUITES "${SUITE}" SUITE_ENABLED)
    set(REQUIRED_TEST OFF)
    if(AVAILABILITY STREQUAL "retained")
        set(REQUIRED_TEST ON)
    elseif(AVAILABILITY STREQUAL "conditional")
        list(GET MANIFEST_NAMES ${MANIFEST_INDEX} CURRENT_NAME)
        list(FIND REQUIRED_CONDITIONAL_TESTS "${CURRENT_NAME}"
            CONDITIONAL_REQUIRED_INDEX)
        if(NOT CONDITIONAL_REQUIRED_INDEX EQUAL -1)
            set(REQUIRED_TEST ON)
        endif()
    endif()
    if(SUITE_ENABLED EQUAL -1 OR NOT REQUIRED_TEST)
        continue()
    endif()
    list(GET MANIFEST_NAMES ${MANIFEST_INDEX} CURRENT_NAME)
    list(FIND CTEST_NAMES "${CURRENT_NAME}" CTEST_INDEX)
    if(CTEST_INDEX EQUAL -1)
        message(FATAL_ERROR
            "Required ${SUITE} test is absent from the generated CTest registry: ${CURRENT_NAME}")
    endif()
endforeach()

list(LENGTH CTEST_NAMES CTEST_NAME_COUNT)
message(STATUS
    "CTest manifest audit passed: ${CTEST_NAME_COUNT} registered tests, "
    "enabled suites ${EXPECTED_SUITES}")
