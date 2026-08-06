# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

function(uml_kernel_build_layout)
    cmake_parse_arguments(ARG ""
        "BINARY_DIR;PAGE_VARIANT;OUT_BUILD_DIR;OUT_IMAGE" "" ${ARGN})
    if(NOT ARG_BINARY_DIR OR NOT ARG_PAGE_VARIANT OR
       NOT ARG_OUT_BUILD_DIR OR NOT ARG_OUT_IMAGE)
        message(FATAL_ERROR
            "uml_kernel_build_layout requires BINARY_DIR, PAGE_VARIANT, "
            "OUT_BUILD_DIR, and OUT_IMAGE")
    endif()
    if(NOT ARG_PAGE_VARIANT MATCHES "^(4k|16k)$")
        message(FATAL_ERROR
            "unsupported kernel page variant: ${ARG_PAGE_VARIANT}")
    endif()
    set(_build_dir "${ARG_BINARY_DIR}/kbuild-${ARG_PAGE_VARIANT}")
    cmake_path(NORMAL_PATH _build_dir)
    set(${ARG_OUT_BUILD_DIR} "${_build_dir}" PARENT_SCOPE)
    set(${ARG_OUT_IMAGE} "${_build_dir}/vmlinux" PARENT_SCOPE)
endfunction()
