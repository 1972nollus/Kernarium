# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.27)

if(NOT DEFINED VERIFY_SCRIPT)
    message(FATAL_ERROR "VERIFY_SCRIPT is required")
endif()
include("${VERIFY_SCRIPT}")

if(DEFINED MODE AND MODE STREQUAL "reject")
    uml_verify_dependency_manifest_product(
        SOURCE "${SOURCE_MANIFEST}"
        INSTALLED "${INSTALLED_MANIFEST}"
        PRODUCT apple-gpu
        LABEL fixture)
    message(FATAL_ERROR "Changed product rows were incorrectly accepted")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _suffix)
set(_fixture_root
    "${CMAKE_CURRENT_BINARY_DIR}/manifest-product-fixture-${_suffix}")
file(MAKE_DIRECTORY "${_fixture_root}")

set(_header
    "name\tversion\tproducts\trepository\tcommit\tsource_paths\tpatch_series\n")
set(_gpu_row
    "gpu\t1\tapple-gpu\thttps://example.invalid/gpu.git\t1111111\t.\t-\n")
set(_zstd_row
    "zstd\t1\tios-compression\thttps://example.invalid/zstd.git\t2222222\t.\t-\n")
set(_source_manifest "${_fixture_root}/source.tsv")
set(_unrelated_manifest "${_fixture_root}/unrelated.tsv")
set(_changed_manifest "${_fixture_root}/changed.tsv")
file(WRITE "${_source_manifest}" "${_header}${_gpu_row}${_zstd_row}")
file(WRITE "${_unrelated_manifest}"
    "${_header}${_gpu_row}zstd\t2\tios-compression\thttps://example.invalid/zstd.git\t3333333\t.\t-\n")
file(WRITE "${_changed_manifest}"
    "${_header}gpu\t2\tapple-gpu\thttps://example.invalid/gpu.git\t4444444\t.\t-\n${_zstd_row}")

uml_verify_dependency_manifest_product(
    SOURCE "${_source_manifest}"
    INSTALLED "${_unrelated_manifest}"
    PRODUCT apple-gpu
    LABEL fixture)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DVERIFY_SCRIPT=${VERIFY_SCRIPT}
        -DMODE=reject
        -DSOURCE_MANIFEST=${_source_manifest}
        -DINSTALLED_MANIFEST=${_changed_manifest}
        -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE _changed_status
    OUTPUT_VARIABLE _changed_output
    ERROR_VARIABLE _changed_error)
if(_changed_status EQUAL 0)
    message(FATAL_ERROR "Changed product rows were incorrectly accepted")
endif()
set(_changed_diagnostic "${_changed_output}${_changed_error}")
if(NOT _changed_diagnostic MATCHES
   "fixture dependency manifest rows do not match")
    message(FATAL_ERROR
        "Changed product rows had the wrong diagnostic: "
        "${_changed_diagnostic}")
endif()

file(REMOVE_RECURSE "${_fixture_root}")
message(STATUS "Dependency product manifest verifier tests passed")
