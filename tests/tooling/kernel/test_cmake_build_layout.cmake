# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED LAYOUT_MODULE OR LAYOUT_MODULE STREQUAL "")
    message(FATAL_ERROR "LAYOUT_MODULE is required")
endif()
include("${LAYOUT_MODULE}")

uml_kernel_build_layout(
    BINARY_DIR "/private/tmp/uml-integration-16/kernel"
    PAGE_VARIANT 16k
    OUT_BUILD_DIR BUILD_16K
    OUT_IMAGE IMAGE_16K)
uml_kernel_build_layout(
    BINARY_DIR "/private/tmp/uml-integration-4/kernel"
    PAGE_VARIANT 4k
    OUT_BUILD_DIR BUILD_4K
    OUT_IMAGE IMAGE_4K)
if(BUILD_16K STREQUAL BUILD_4K OR IMAGE_16K STREQUAL IMAGE_4K)
    message(FATAL_ERROR "page-mode integration trees share a kernel output")
endif()
if(NOT BUILD_16K STREQUAL
       "/private/tmp/uml-integration-16/kernel/kbuild-16k" OR
   NOT IMAGE_16K STREQUAL "${BUILD_16K}/vmlinux" OR
   NOT BUILD_4K STREQUAL
       "/private/tmp/uml-integration-4/kernel/kbuild-4k" OR
   NOT IMAGE_4K STREQUAL "${BUILD_4K}/vmlinux")
    message(FATAL_ERROR "kernel build layout is not binary-tree local")
endif()

uml_kernel_build_layout(
    BINARY_DIR "/private/tmp/uml-one-config/kernel"
    PAGE_VARIANT 16k
    OUT_BUILD_DIR SAME_TREE_16K
    OUT_IMAGE SAME_TREE_IMAGE_16K)
uml_kernel_build_layout(
    BINARY_DIR "/private/tmp/uml-one-config/kernel"
    PAGE_VARIANT 4k
    OUT_BUILD_DIR SAME_TREE_4K
    OUT_IMAGE SAME_TREE_IMAGE_4K)
if(SAME_TREE_16K STREQUAL SAME_TREE_4K OR
   SAME_TREE_IMAGE_16K STREQUAL SAME_TREE_IMAGE_4K)
    message(FATAL_ERROR "page variants collide inside one binary tree")
endif()
