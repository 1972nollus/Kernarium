# SPDX-License-Identifier: GPL-2.0-or-later

if(UML_GUEST_16K_PAGES)
    set(UML_KERNEL_PAGE_VARIANT 16k)
    set(UML_KERNEL_PAGE_FRAGMENT
        "${PROJECT_SOURCE_DIR}/kernel/config/page-16k.fragment")
else()
    set(UML_KERNEL_PAGE_VARIANT 4k)
    set(UML_KERNEL_PAGE_FRAGMENT
        "${PROJECT_SOURCE_DIR}/kernel/config/page-4k.fragment")
endif()

# By default, each CMake binary tree owns a page-variant-specific Kbuild
# directory. The canonical integration presets override that default with
# dedicated 16 KiB and 4 KiB trees on /Volumes/Kernel, so concurrent
# builds still never reuse or replace one another's .config, objects, or
# embedded vmlinux.
include("${PROJECT_SOURCE_DIR}/kernel/CMakeBuildLayout.cmake")
uml_kernel_build_layout(
    BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}"
    PAGE_VARIANT "${UML_KERNEL_PAGE_VARIANT}"
    OUT_BUILD_DIR UML_KERNEL_DEFAULT_BUILD_DIR
    OUT_IMAGE UML_KERNEL_DEFAULT_IMAGE)
set(UML_KERNEL_BUILD_DIR "${UML_KERNEL_DEFAULT_BUILD_DIR}" CACHE PATH
    "Out-of-tree Linux UML kernel build directory")
# The kernel image is an output of UML_KERNEL_BUILD_DIR, not an independent
# input. Keeping it as a normal user cache entry allowed a reused build tree
# to compile one kernel while embedding a stale image from another directory.
set(UML_KERNEL_IMAGE "${UML_KERNEL_BUILD_DIR}/vmlinux" CACHE INTERNAL
    "Derived Linux UML kernel ELF embedded in the transitional macOS CLI" FORCE)
set(UML_KERNEL_SOURCE_DIR "${PROJECT_SOURCE_DIR}/linux" CACHE PATH
    "Prepared official Linux UML source checkout")
if(UML_KERNEL_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR
        "UML_KERNEL_SOURCE_DIR is empty. Name a clean prepared Linux checkout "
        "on a case-sensitive filesystem; canonical integration presets pin "
        "/Volumes/Kernel/linux.")
endif()
if(NOT EXISTS "${UML_KERNEL_SOURCE_DIR}/Makefile")
    message(FATAL_ERROR
        "UML_KERNEL_SOURCE_DIR must contain a prepared Linux checkout: "
        "${UML_KERNEL_SOURCE_DIR}")
endif()
execute_process(
    COMMAND "${PROJECT_SOURCE_DIR}/kernel/prepare-source.sh"
        --source "${UML_KERNEL_SOURCE_DIR}" --verify --no-fetch
    RESULT_VARIABLE UML_KERNEL_SOURCE_VERIFY_RESULT
    OUTPUT_VARIABLE UML_KERNEL_SOURCE_VERIFY_OUTPUT
    ERROR_VARIABLE UML_KERNEL_SOURCE_VERIFY_ERROR)
if(NOT UML_KERNEL_SOURCE_VERIFY_RESULT EQUAL 0)
    message(FATAL_ERROR
        "UML_KERNEL_SOURCE_DIR is not the clean canonical replay:\n"
        "${UML_KERNEL_SOURCE_VERIFY_OUTPUT}${UML_KERNEL_SOURCE_VERIFY_ERROR}")
endif()
execute_process(
    COMMAND "${PROJECT_SOURCE_DIR}/kernel/audit-delta.sh"
        --source "${UML_KERNEL_SOURCE_DIR}" --tree HEAD
    RESULT_VARIABLE UML_KERNEL_DELTA_AUDIT_RESULT
    OUTPUT_VARIABLE UML_KERNEL_DELTA_AUDIT_OUTPUT
    ERROR_VARIABLE UML_KERNEL_DELTA_AUDIT_ERROR)
if(NOT UML_KERNEL_DELTA_AUDIT_RESULT EQUAL 0)
    message(FATAL_ERROR
        "UML kernel delta policy failed:\n"
        "${UML_KERNEL_DELTA_AUDIT_OUTPUT}${UML_KERNEL_DELTA_AUDIT_ERROR}")
endif()
find_program(UML_KERNEL_GIT_EXECUTABLE NAMES git REQUIRED)
# Kbuild's input is a separate prepared checkout, so CMake cannot discover its
# source-file dependencies itself. Watch the ref that identifies the selected
# clean replay: advancing it must rebuild the embedded ELF instead of silently
# reusing a vmlinux produced from the previous commit. A detached replay uses
# HEAD directly; an attached managed branch uses its ref file.
execute_process(
    COMMAND ${UML_KERNEL_GIT_EXECUTABLE} -C ${UML_KERNEL_SOURCE_DIR}
        symbolic-ref --quiet HEAD
    RESULT_VARIABLE UML_KERNEL_SYMBOLIC_REF_RESULT
    OUTPUT_VARIABLE UML_KERNEL_SYMBOLIC_REF
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(UML_KERNEL_SYMBOLIC_REF_RESULT EQUAL 0)
    execute_process(
        COMMAND ${UML_KERNEL_GIT_EXECUTABLE} -C ${UML_KERNEL_SOURCE_DIR}
            rev-parse --git-path ${UML_KERNEL_SYMBOLIC_REF}
        RESULT_VARIABLE UML_KERNEL_REVISION_FILE_RESULT
        OUTPUT_VARIABLE UML_KERNEL_REVISION_FILE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
else()
    execute_process(
        COMMAND ${UML_KERNEL_GIT_EXECUTABLE} -C ${UML_KERNEL_SOURCE_DIR}
            rev-parse --git-path HEAD
        RESULT_VARIABLE UML_KERNEL_REVISION_FILE_RESULT
        OUTPUT_VARIABLE UML_KERNEL_REVISION_FILE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT IS_ABSOLUTE "${UML_KERNEL_REVISION_FILE}")
    get_filename_component(UML_KERNEL_REVISION_FILE
        "${UML_KERNEL_REVISION_FILE}" ABSOLUTE
        BASE_DIR "${UML_KERNEL_SOURCE_DIR}")
endif()
if(NOT UML_KERNEL_REVISION_FILE_RESULT EQUAL 0 OR
   NOT EXISTS "${UML_KERNEL_REVISION_FILE}")
    message(FATAL_ERROR
        "Cannot resolve the Git revision file for UML_KERNEL_SOURCE_DIR: "
        "${UML_KERNEL_SOURCE_DIR}")
endif()
set(UML_KERNEL_JOBS "" CACHE STRING
    "Parallel GNU Make jobs; empty uses all online host processors")
if(NOT UML_KERNEL_JOBS STREQUAL "" AND
   NOT UML_KERNEL_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "UML_KERNEL_JOBS must be empty or a positive integer")
endif()
find_program(UML_KERNEL_MAKE_EXECUTABLE NAMES gmake make REQUIRED)

# Apple Clang releases can accept __counted_by while failing on the type
# expressions Linux 6.18 uses. Select by capability during configure rather
# than allowing a long Kbuild to fail later. Explicit overrides are
# authoritative; otherwise a caller-supplied toolchain root precedes the
# configured compiler and PATH. LLVM companion tools come from the selected
# compiler directory so an unrelated PATH entry cannot silently mix versions.
include("${PROJECT_SOURCE_DIR}/kernel/KernelToolchain.cmake")
set(_uml_kernel_toolchain_root_default "$ENV{UML_KERNEL_TOOLCHAIN_ROOT}")
set(UML_KERNEL_TOOLCHAIN_ROOT "${_uml_kernel_toolchain_root_default}" CACHE PATH
    "LLVM installation root used for the Linux kernel build")
set(_uml_kernel_cc_default "$ENV{UML_KERNEL_CC}")
set(UML_KERNEL_CC "${_uml_kernel_cc_default}" CACHE FILEPATH
    "Explicit Linux kernel compiler override")
set(_uml_kernel_host_cc_default "$ENV{UML_KERNEL_HOST_CC}")
set(UML_KERNEL_HOST_CC "${_uml_kernel_host_cc_default}" CACHE FILEPATH
    "Explicit Linux host-tool compiler override")
set(_uml_kernel_ld_default "$ENV{UML_KERNEL_LD}")
set(UML_KERNEL_LD "${_uml_kernel_ld_default}" CACHE FILEPATH
    "Explicit Linux kernel linker override")
foreach(_uml_kernel_tool IN ITEMS AR NM OBJCOPY OBJDUMP READELF STRIP)
    set(_uml_kernel_tool_default "$ENV{UML_KERNEL_${_uml_kernel_tool}}")
    set(UML_KERNEL_${_uml_kernel_tool} "${_uml_kernel_tool_default}"
        CACHE FILEPATH
        "Explicit Linux kernel llvm tool override")
endforeach()

uml_select_kernel_toolchain(
    WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}/toolchain-probes"
    TOOLCHAIN_ROOT "${UML_KERNEL_TOOLCHAIN_ROOT}"
    CONFIGURED_CC "${CMAKE_C_COMPILER}"
    COMPILER_CANDIDATES clang
    REQUESTED_CC "${UML_KERNEL_CC}"
    REQUESTED_HOST_CC "${UML_KERNEL_HOST_CC}"
    REQUESTED_LD "${UML_KERNEL_LD}"
    REQUESTED_AR "${UML_KERNEL_AR}"
    REQUESTED_NM "${UML_KERNEL_NM}"
    REQUESTED_OBJCOPY "${UML_KERNEL_OBJCOPY}"
    REQUESTED_OBJDUMP "${UML_KERNEL_OBJDUMP}"
    REQUESTED_READELF "${UML_KERNEL_READELF}"
    REQUESTED_STRIP "${UML_KERNEL_STRIP}"
    OUT_CC _uml_kernel_selected_cc
    OUT_HOST_CC _uml_kernel_selected_host_cc
    OUT_LD _uml_kernel_selected_ld
    OUT_AR _uml_kernel_selected_ar
    OUT_NM _uml_kernel_selected_nm
    OUT_OBJCOPY _uml_kernel_selected_objcopy
    OUT_OBJDUMP _uml_kernel_selected_objdump
    OUT_READELF _uml_kernel_selected_readelf
    OUT_STRIP _uml_kernel_selected_strip)
set(UML_KERNEL_CC_EXECUTABLE "${_uml_kernel_selected_cc}" CACHE INTERNAL
    "Capability-checked Linux kernel compiler" FORCE)
set(UML_KERNEL_HOST_CC_EXECUTABLE "${_uml_kernel_selected_host_cc}"
    CACHE INTERNAL "Capability-checked Linux host-tool compiler" FORCE)
set(UML_KERNEL_LD_EXECUTABLE "${_uml_kernel_selected_ld}" CACHE INTERNAL
    "Resolved Linux kernel linker" FORCE)
foreach(_uml_kernel_tool IN ITEMS AR NM OBJCOPY OBJDUMP READELF STRIP)
    string(TOLOWER "${_uml_kernel_tool}" _uml_kernel_tool_name)
    set(UML_KERNEL_${_uml_kernel_tool}_EXECUTABLE
        "${_uml_kernel_selected_${_uml_kernel_tool_name}}" CACHE INTERNAL
        "Resolved Linux kernel llvm-${_uml_kernel_tool_name} tool" FORCE)
endforeach()

# Kbuild's deterministic archive flag is unsupported by Apple ar. Requiring
# the companion LLVM tools beside the selected compiler prevents a late build
# failure and keeps automatic selection within one coherent tool family.
set(UML_KERNEL_CROSS_COMPILE "" CACHE STRING
    "Optional tool prefix passed to Kbuild (Darwin UML is native by default)")
set(UML_KERNEL_LLVM "" CACHE STRING
    "Optional Kbuild LLVM selector (for example 1 or a tool-prefix)")

add_custom_target(uml_kernel_source_verify
    COMMAND "${PROJECT_SOURCE_DIR}/kernel/prepare-source.sh"
        --source "${UML_KERNEL_SOURCE_DIR}" --verify --no-fetch
    COMMAND "${PROJECT_SOURCE_DIR}/kernel/audit-delta.sh"
        --source "${UML_KERNEL_SOURCE_DIR}" --tree HEAD
    COMMENT "Verifying clean canonical Linux replay and delta policy"
    VERBATIM)

file(GLOB UML_KERNEL_PATCH_FILES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/kernel/patches/series/*.patch")

add_custom_command(
    OUTPUT "${UML_KERNEL_IMAGE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${UML_KERNEL_BUILD_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env
            "UML_KERNEL_MAKE=${UML_KERNEL_MAKE_EXECUTABLE}"
            "UML_KERNEL_CC=${UML_KERNEL_CC_EXECUTABLE}"
            "UML_KERNEL_HOST_CC=${UML_KERNEL_HOST_CC_EXECUTABLE}"
            "UML_KERNEL_LD=${UML_KERNEL_LD_EXECUTABLE}"
            "UML_KERNEL_CROSS_COMPILE=${UML_KERNEL_CROSS_COMPILE}"
            "UML_KERNEL_LLVM=${UML_KERNEL_LLVM}"
            "UML_KERNEL_JOBS=${UML_KERNEL_JOBS}"
            "UML_KERNEL_AR=${UML_KERNEL_AR_EXECUTABLE}"
            "UML_KERNEL_NM=${UML_KERNEL_NM_EXECUTABLE}"
            "UML_KERNEL_OBJCOPY=${UML_KERNEL_OBJCOPY_EXECUTABLE}"
            "UML_KERNEL_OBJDUMP=${UML_KERNEL_OBJDUMP_EXECUTABLE}"
            "UML_KERNEL_READELF=${UML_KERNEL_READELF_EXECUTABLE}"
            "UML_KERNEL_STRIP=${UML_KERNEL_STRIP_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/kernel/build-kernel.sh"
            --source "${UML_KERNEL_SOURCE_DIR}"
            --build "${UML_KERNEL_BUILD_DIR}"
            --page-size "${UML_KERNEL_PAGE_VARIANT}"
    DEPENDS
            "${PROJECT_SOURCE_DIR}/kernel/config/common.fragment"
            "${UML_KERNEL_PAGE_FRAGMENT}"
            "${PROJECT_SOURCE_DIR}/kernel/build-kernel.sh"
            "${PROJECT_SOURCE_DIR}/kernel/KernelToolchain.cmake"
            "${PROJECT_SOURCE_DIR}/kernel/toolchain/darwin.env"
            "${PROJECT_SOURCE_DIR}/kernel/toolchain/darwin-host/host_compat.h"
            "${PROJECT_SOURCE_DIR}/kernel/toolchain/darwin-host/include/byteswap.h"
            "${PROJECT_SOURCE_DIR}/kernel/toolchain/darwin-host/include/elf.h"
            "${PROJECT_SOURCE_DIR}/kernel/toolchain/darwin-host/include/libelf.h"
            "${PROJECT_SOURCE_DIR}/kernel/toolchain/darwin-host/bin/find"
            "${PROJECT_SOURCE_DIR}/kernel/abi/sync.sh"
            "${PROJECT_SOURCE_DIR}/kernel/abi/verify-linux-imports.sh"
            "${PROJECT_SOURCE_DIR}/kernel/abi/import-policy.tsv"
            "${PROJECT_SOURCE_DIR}/kernel/abi/darwin_uml_abi.h"
            "${PROJECT_SOURCE_DIR}/kernel/abi/darwin_uml_bridge.h"
            "${PROJECT_SOURCE_DIR}/kernel/abi/darwin_uml_cpu.h"
            "${PROJECT_SOURCE_DIR}/kernel/abi/darwin_uml_host.h"
            "${PROJECT_SOURCE_DIR}/kernel/abi/darwin_uml_memory.h"
            "${PROJECT_SOURCE_DIR}/kernel/abi/darwin_uml_network.h"
            "${PROJECT_SOURCE_DIR}/kernel/prepare-source.sh"
            "${PROJECT_SOURCE_DIR}/kernel/materialize-release.sh"
            "${PROJECT_SOURCE_DIR}/kernel/audit-delta.sh"
            "${PROJECT_SOURCE_DIR}/kernel/verify-series-policy.py"
            "${PROJECT_SOURCE_DIR}/kernel/verify-replay.sh"
            "${PROJECT_SOURCE_DIR}/kernel/check-next-version.sh"
            "${PROJECT_SOURCE_DIR}/kernel/patches/metadata.env"
            "${PROJECT_SOURCE_DIR}/kernel/keys/38DBBDC86092693E.asc"
            "${PROJECT_SOURCE_DIR}/kernel/patches/path-ownership.tsv"
            "${PROJECT_SOURCE_DIR}/kernel/patches/extraction-decisions.tsv"
            "${PROJECT_SOURCE_DIR}/kernel/patches/outside-port-justification.tsv"
            "${PROJECT_SOURCE_DIR}/kernel/patches/topic-path-overlaps.tsv"
            "${PROJECT_SOURCE_DIR}/kernel/patches/series/manifest.tsv"
            ${UML_KERNEL_PATCH_FILES}
            uml_kernel_source_verify
            "${UML_KERNEL_REVISION_FILE}"
    COMMENT "Building the ${UML_KERNEL_PAGE_VARIANT} Darwin UML kernel"
    VERBATIM)

add_custom_target(uml_kernel_config
    DEPENDS "${UML_KERNEL_IMAGE}")
add_dependencies(uml_kernel_config uml_kernel_source_verify)

add_custom_target(uml_kernel
    DEPENDS "${UML_KERNEL_IMAGE}")
add_dependencies(uml_kernel uml_kernel_source_verify)
