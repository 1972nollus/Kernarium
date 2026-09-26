# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED PREFIX)
    message(FATAL_ERROR "PREFIX is required for kernel host tool verification")
endif()
if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR
        "SOURCE_ROOT is required for kernel host tool verification")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/VerifyManifestProduct.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/VerifySourceTrees.cmake")

set(_required
    "${PREFIX}/bin/pahole"
    "${PREFIX}/include/elf.h"
    "${PREFIX}/include/gelf.h"
    "${PREFIX}/include/libelf.h"
    "${PREFIX}/include/nlist.h"
    "${PREFIX}/lib/libelf.a"
    "${PREFIX}/lib/pkgconfig/libelf.pc")
foreach(_file IN LISTS _required)
    if(NOT EXISTS "${_file}" OR IS_SYMLINK "${_file}")
        message(FATAL_ERROR "Invalid kernel host tools prefix: ${_file}")
    endif()
endforeach()

set(_manifest "${SOURCE_ROOT}/dependencies/manifest.tsv")
uml_verify_dependency_manifest_product(
    SOURCE "${_manifest}"
    INSTALLED "${PREFIX}/share/uml-dependencies/manifest.tsv"
    PRODUCT kernel-host-tools
    LABEL "Kernel host tools")
uml_verify_source_trees(
    INSTALLED "${PREFIX}/share/uml-dependencies/source-trees.tsv"
    LABEL "Kernel host tools"
    SOURCES
        musl-obstack "${SOURCE_ROOT}/dependencies/patches/musl-obstack/series.tsv"
        elfutils "${SOURCE_ROOT}/dependencies/patches/elfutils/series.tsv"
        libbpf "${SOURCE_ROOT}/dependencies/patches/libbpf/series.tsv"
        dwarves "${SOURCE_ROOT}/dependencies/patches/dwarves/series.tsv")

set(_archives_installed
    "${PREFIX}/share/uml-dependencies/kernel-host-tools-archives.tsv")
if(NOT EXISTS "${_archives_installed}" OR IS_SYMLINK "${_archives_installed}")
    message(FATAL_ERROR
        "Missing kernel host tools archive provenance: ${_archives_installed}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${SOURCE_ROOT}/dependencies/kernel-host-tools/archives.tsv"
        "${_archives_installed}"
    RESULT_VARIABLE _archives_status)
if(NOT _archives_status EQUAL 0)
    message(FATAL_ERROR "Kernel host tools archive provenance does not match")
endif()

execute_process(
    COMMAND lipo -archs "${PREFIX}/bin/pahole"
    RESULT_VARIABLE _lipo_status
    OUTPUT_VARIABLE _architectures
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _lipo_status EQUAL 0 OR NOT _architectures STREQUAL "arm64")
    message(FATAL_ERROR "pahole is not an arm64 executable: ${_architectures}")
endif()

# The prefix must report the pinned dwarves release as its pahole version,
# since Kbuild derives its BTF options from that version.
file(STRINGS "${_manifest}" _dwarves_rows REGEX "^dwarves\t")
list(LENGTH _dwarves_rows _dwarves_count)
if(NOT _dwarves_count EQUAL 1 OR
        NOT _dwarves_rows MATCHES "^dwarves\t([0-9]+\\.[0-9]+)\t")
    message(FATAL_ERROR "The manifest pins no single dwarves release")
endif()
set(_expected_version "v${CMAKE_MATCH_1}")
execute_process(
    COMMAND "${PREFIX}/bin/pahole" --version
    RESULT_VARIABLE _pahole_status
    OUTPUT_VARIABLE _pahole_version
    ERROR_VARIABLE _pahole_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _pahole_status EQUAL 0 OR
        NOT _pahole_version STREQUAL _expected_version)
    message(FATAL_ERROR
        "pahole reports '${_pahole_version}${_pahole_error}', "
        "expected ${_expected_version}")
endif()
