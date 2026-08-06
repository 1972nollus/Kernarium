# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED SOURCE_SIZE_MODULE OR SOURCE_SIZE_MODULE STREQUAL "")
    message(FATAL_ERROR "SOURCE_SIZE_MODULE is required")
endif()
if(NOT DEFINED TEST_ROOT OR TEST_ROOT STREQUAL "")
    message(FATAL_ERROR "TEST_ROOT is required")
endif()
get_filename_component(TEST_ROOT_NAME "${TEST_ROOT}" NAME)
if(NOT TEST_ROOT_NAME MATCHES "^source-size-policy-fixture")
    message(FATAL_ERROR
        "Refusing to replace an unexpected source-size fixture: ${TEST_ROOT}")
endif()

include("${SOURCE_SIZE_MODULE}")

function(_uml_write_source_size_fixture PATH LINE_COUNT)
    get_filename_component(PARENT_DIRECTORY "${PATH}" DIRECTORY)
    file(MAKE_DIRECTORY "${PARENT_DIRECTORY}")
    string(REPEAT "x\n" ${LINE_COUNT} CONTENTS)
    file(WRITE "${PATH}" "${CONTENTS}")
endfunction()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/docs")
_uml_write_source_size_fixture("${TEST_ROOT}/cmake/policy.cmake" 301)
_uml_write_source_size_fixture("${TEST_ROOT}/darwin/large.cpp" 1001)
file(MAKE_DIRECTORY "${TEST_ROOT}/darwin/rootfs")
file(WRITE "${TEST_ROOT}/darwin/rootfs/mkfs.ext2"
    "#!/bin/sh\nexit 0\n")
_uml_write_source_size_fixture("${TEST_ROOT}/dependencies/build.sh" 601)
_uml_write_source_size_fixture("${TEST_ROOT}/dependencies/inventory.tsv" 1001)
_uml_write_source_size_fixture("${TEST_ROOT}/kernel/helper.hpp" 2)
_uml_write_source_size_fixture("${TEST_ROOT}/scripts/helper.sh" 2)
_uml_write_source_size_fixture("${TEST_ROOT}/tests/large.cpp" 601)
_uml_write_source_size_fixture("${TEST_ROOT}/tests/runner.py" 2)
_uml_write_source_size_fixture("${TEST_ROOT}/tests/CMakeLists.txt" 301)
_uml_write_source_size_fixture("${TEST_ROOT}/tests/inventory.tsv" 1001)
_uml_write_source_size_fixture("${TEST_ROOT}/tests/model.asl" 1001)
_uml_write_source_size_fixture("${TEST_ROOT}/tools/helper.py" 2)
file(MAKE_DIRECTORY "${TEST_ROOT}/tools")
file(WRITE "${TEST_ROOT}/tools/extensionless-helper"
    "#!/bin/sh\nexit 0\n")
file(WRITE "${TEST_ROOT}/tests/no-final-newline.sh" "first\n\nthird")

set(REVIEW_MANIFEST "${TEST_ROOT}/docs/source-size-reviews.tsv")
file(WRITE "${REVIEW_MANIFEST}"
    "path\towner\treviewed_lines\tnote\treviewed\n"
    "darwin/large.cpp\tCPU\t900\tcohesive\t2026-08-03\n"
    "tests/runner.py\t-\t2\t-\t-\n")
set(REPORT_PATH "${TEST_ROOT}/source-sizes.tsv")
uml_report_source_sizes(
    SOURCE_ROOT "${TEST_ROOT}"
    REVIEW_MANIFEST "${REVIEW_MANIFEST}"
    REPORT_PATH "${REPORT_PATH}"
    OUT_ERRORS REPORT_ERRORS)
if(REPORT_ERRORS)
    list(JOIN REPORT_ERRORS "\n  " FORMATTED_REPORT_ERRORS)
    message(FATAL_ERROR
        "Valid advisory review records were rejected:\n  ${FORMATTED_REPORT_ERRORS}")
endif()

file(READ "${REPORT_PATH}" REPORT_CONTENTS)
set(EXPECTED_ROWS
    "cmake\t300\t301\t1\tcmake/policy.cmake\tabove-guideline-unreviewed\t-\t-\t-\t-"
    "production\t800\t1001\t201\tdarwin/large.cpp\tchanged-since-review\tCPU\t900\t2026-08-03\tcohesive"
    "support\t600\t2\t0\tdarwin/rootfs/mkfs.ext2\twithin-guideline\t-\t-\t-\t-"
    "support\t600\t601\t1\tdependencies/build.sh\tabove-guideline-unreviewed\t-\t-\t-\t-"
    "declarative\t-\t1001\t-\tdependencies/inventory.tsv\tinformational\t-\t-\t-\t-"
    "support\t600\t2\t0\tkernel/helper.hpp\twithin-guideline\t-\t-\t-\t-"
    "support\t600\t2\t0\tscripts/helper.sh\twithin-guideline\t-\t-\t-\t-"
    "cmake\t300\t301\t1\ttests/CMakeLists.txt\tabove-guideline-unreviewed\t-\t-\t-\t-"
    "declarative\t-\t1001\t-\ttests/inventory.tsv\tinformational\t-\t-\t-\t-"
    "declarative\t-\t1001\t-\ttests/model.asl\tinformational\t-\t-\t-\t-"
    "test\t600\t601\t1\ttests/large.cpp\tabove-guideline-unreviewed\t-\t-\t-\t-"
    "test\t600\t3\t0\ttests/no-final-newline.sh\twithin-guideline\t-\t-\t-\t-"
    "test\t600\t2\t0\ttests/runner.py\treviewed\t-\t2\t-\t-"
    "support\t600\t2\t0\ttools/extensionless-helper\twithin-guideline\t-\t-\t-\t-"
    "support\t600\t2\t0\ttools/helper.py\twithin-guideline\t-\t-\t-\t-")
foreach(EXPECTED_ROW IN LISTS EXPECTED_ROWS)
    string(FIND "${REPORT_CONTENTS}" "${EXPECTED_ROW}" ROW_INDEX)
    if(ROW_INDEX EQUAL -1)
        message(FATAL_ERROR
            "Source-size report is missing expected row: ${EXPECTED_ROW}")
    endif()
endforeach()

# Review metadata remains enforceable independently from the advisory size
# status. Duplicate or stale records are returned to the architecture audit as
# errors, while a size crossing itself never becomes an error.
set(INVALID_MANIFEST "${TEST_ROOT}/docs/invalid-source-size-reviews.tsv")
file(WRITE "${INVALID_MANIFEST}"
    "path\towner\treviewed_lines\tnote\treviewed\n"
    "darwin/large.cpp\tCPU\t1001\tfirst\t2026-08-03\n"
    "darwin/large.cpp\tCPU\t1001\tduplicate\t2026-08-03\n")
uml_report_source_sizes(
    SOURCE_ROOT "${TEST_ROOT}"
    REVIEW_MANIFEST "${INVALID_MANIFEST}"
    OUT_ERRORS INVALID_REPORT_ERRORS)
if(NOT INVALID_REPORT_ERRORS)
    message(FATAL_ERROR "Duplicate review metadata was not reported")
endif()
