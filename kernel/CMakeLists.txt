# SPDX-License-Identifier: GPL-2.0-or-later

set(UML_KERNEL_BUILD_DIR "${PROJECT_SOURCE_DIR}/.build/kernel" CACHE PATH
    "Out-of-tree Linux UML kernel build directory")
# The kernel image is an output of UML_KERNEL_BUILD_DIR, not an independent
# input. Keeping it as a normal user cache entry allowed a reused build tree
# to compile one kernel while embedding a stale image from another directory.
set(UML_KERNEL_IMAGE "${UML_KERNEL_BUILD_DIR}/vmlinux" CACHE INTERNAL
    "Derived Linux UML kernel ELF embedded in the transitional macOS CLI" FORCE)
set(UML_KERNEL_SOURCE_DIR "${PROJECT_SOURCE_DIR}/linux" CACHE PATH
    "Prepared official Linux UML source checkout")
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
set(UML_KERNEL_JOBS 8 CACHE STRING
    "Parallel GNU Make jobs for the out-of-tree UML kernel build")
if(NOT UML_KERNEL_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "UML_KERNEL_JOBS must be a positive integer")
endif()
find_program(UML_KERNEL_MAKE_EXECUTABLE NAMES gmake make REQUIRED)
find_program(UML_KERNEL_CC_EXECUTABLE NAMES clang REQUIRED)
find_program(UML_KERNEL_HOST_CC_EXECUTABLE NAMES clang REQUIRED)
find_program(UML_KERNEL_LD_EXECUTABLE NAMES ld.lld REQUIRED)
get_filename_component(UML_KERNEL_TOOLCHAIN_DIR
    "${UML_KERNEL_CC_EXECUTABLE}" DIRECTORY)

# Prefer the LLVM binutils shipped beside a selected LLVM clang. Kbuild's
# standard archive flags are not supported by Apple ar; this compatibility
# choice is not a project promise that separate builds have identical hashes.
# Users can still set any cache override explicitly for a nonstandard
# toolchain.
function(uml_default_kernel_llvm_tool variable tool)
    set(_uml_kernel_tool_candidate_variable "_uml_kernel_${tool}_candidate")
    find_program(${_uml_kernel_tool_candidate_variable}
        NAMES "llvm-${tool}"
        HINTS "${UML_KERNEL_TOOLCHAIN_DIR}"
        NO_DEFAULT_PATH)
    if(${_uml_kernel_tool_candidate_variable})
        set(${variable} "${${_uml_kernel_tool_candidate_variable}}" PARENT_SCOPE)
    else()
        set(${variable} "" PARENT_SCOPE)
    endif()
endfunction()
uml_default_kernel_llvm_tool(UML_KERNEL_AR_DEFAULT ar)
uml_default_kernel_llvm_tool(UML_KERNEL_NM_DEFAULT nm)
uml_default_kernel_llvm_tool(UML_KERNEL_OBJCOPY_DEFAULT objcopy)
uml_default_kernel_llvm_tool(UML_KERNEL_OBJDUMP_DEFAULT objdump)
uml_default_kernel_llvm_tool(UML_KERNEL_READELF_DEFAULT readelf)
uml_default_kernel_llvm_tool(UML_KERNEL_STRIP_DEFAULT strip)
set(UML_KERNEL_CROSS_COMPILE "" CACHE STRING
    "Optional tool prefix passed to Kbuild (Darwin UML is native by default)")
set(UML_KERNEL_LLVM "" CACHE STRING
    "Optional Kbuild LLVM selector (for example 1 or a tool-prefix)")
set(UML_KERNEL_AR "${UML_KERNEL_AR_DEFAULT}" CACHE FILEPATH
    "Archiver override passed to Kbuild")
set(UML_KERNEL_NM "${UML_KERNEL_NM_DEFAULT}" CACHE FILEPATH
    "nm override passed to Kbuild")
set(UML_KERNEL_OBJCOPY "${UML_KERNEL_OBJCOPY_DEFAULT}" CACHE FILEPATH
    "objcopy override passed to Kbuild")
set(UML_KERNEL_OBJDUMP "${UML_KERNEL_OBJDUMP_DEFAULT}" CACHE FILEPATH
    "objdump override passed to Kbuild")
set(UML_KERNEL_READELF "${UML_KERNEL_READELF_DEFAULT}" CACHE FILEPATH
    "readelf override passed to Kbuild")
set(UML_KERNEL_STRIP "${UML_KERNEL_STRIP_DEFAULT}" CACHE FILEPATH
    "strip override passed to Kbuild")
set(UML_KERNEL_PROFILE production CACHE STRING
    "Darwin UML kernel policy profile (core, desktop, or production)")
set_property(CACHE UML_KERNEL_PROFILE PROPERTY STRINGS core desktop production)
if(NOT UML_KERNEL_PROFILE MATCHES "^(core|desktop|production)$")
    message(FATAL_ERROR
        "UML_KERNEL_PROFILE must be core, desktop, or production")
endif()

if(UML_GUEST_16K_PAGES)
    set(UML_KERNEL_PAGE_VARIANT 16k)
    set(UML_KERNEL_PAGE_FRAGMENT
        "${PROJECT_SOURCE_DIR}/kernel/config/page-16k.fragment")
else()
    set(UML_KERNEL_PAGE_VARIANT 4k)
    set(UML_KERNEL_PAGE_FRAGMENT
        "${PROJECT_SOURCE_DIR}/kernel/config/page-4k.fragment")
endif()

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
            "UML_KERNEL_AR=${UML_KERNEL_AR}"
            "UML_KERNEL_NM=${UML_KERNEL_NM}"
            "UML_KERNEL_OBJCOPY=${UML_KERNEL_OBJCOPY}"
            "UML_KERNEL_OBJDUMP=${UML_KERNEL_OBJDUMP}"
            "UML_KERNEL_READELF=${UML_KERNEL_READELF}"
            "UML_KERNEL_STRIP=${UML_KERNEL_STRIP}"
            "${PROJECT_SOURCE_DIR}/kernel/build-kernel.sh"
            --source "${UML_KERNEL_SOURCE_DIR}"
            --build "${UML_KERNEL_BUILD_DIR}"
            --page-size "${UML_KERNEL_PAGE_VARIANT}"
            --profile "${UML_KERNEL_PROFILE}"
    DEPENDS
            "${PROJECT_SOURCE_DIR}/kernel/config/common.fragment"
            "${PROJECT_SOURCE_DIR}/kernel/config/desktop.fragment"
            "${PROJECT_SOURCE_DIR}/kernel/config/containers.fragment"
            "${UML_KERNEL_PAGE_FRAGMENT}"
            "${PROJECT_SOURCE_DIR}/kernel/build-kernel.sh"
            "${PROJECT_SOURCE_DIR}/kernel/toolchain/darwin.env"
            "${PROJECT_SOURCE_DIR}/kernel/toolchain/darwin-host/host_compat.h"
            "${PROJECT_SOURCE_DIR}/kernel/toolchain/darwin-host/include/libelf.h"
            "${PROJECT_SOURCE_DIR}/kernel/toolchain/darwin-host/bin/find"
            "${PROJECT_SOURCE_DIR}/kernel/abi/sync.sh"
            "${PROJECT_SOURCE_DIR}/kernel/abi/darwin_uml_abi.h"
            "${PROJECT_SOURCE_DIR}/kernel/abi/darwin_uml_bridge.h"
            "${PROJECT_SOURCE_DIR}/kernel/abi/darwin_uml_cpu.h"
            "${PROJECT_SOURCE_DIR}/kernel/abi/darwin_uml_host.h"
            "${PROJECT_SOURCE_DIR}/kernel/abi/darwin_uml_memory.h"
            "${PROJECT_SOURCE_DIR}/kernel/abi/darwin_uml_network.h"
            "${PROJECT_SOURCE_DIR}/kernel/prepare-source.sh"
            "${PROJECT_SOURCE_DIR}/kernel/materialize-release.sh"
            "${PROJECT_SOURCE_DIR}/kernel/audit-delta.sh"
            "${PROJECT_SOURCE_DIR}/kernel/verify-replay.sh"
            "${PROJECT_SOURCE_DIR}/kernel/check-next-version.sh"
            "${PROJECT_SOURCE_DIR}/kernel/patches/metadata.env"
            "${PROJECT_SOURCE_DIR}/kernel/keys/38DBBDC86092693E.asc"
            "${PROJECT_SOURCE_DIR}/kernel/patches/path-ownership.tsv"
            "${PROJECT_SOURCE_DIR}/kernel/patches/outside-port-justification.tsv"
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
