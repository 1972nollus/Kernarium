# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME kernel-release-source-preparer
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_materialize_release.sh
    LABELS kernel;dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-series-topology
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_series_topology.sh
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-cmake-build-layout
    COMMAND ${CMAKE_COMMAND}
        -DLAYOUT_MODULE=${PROJECT_SOURCE_DIR}/kernel/CMakeBuildLayout.cmake
        -P ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_cmake_build_layout.cmake
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-compiler-selection
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_kernel_toolchain.sh
        ${PROJECT_SOURCE_DIR}/kernel/KernelToolchain.cmake
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-darwin-host-compat
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_darwin_host_compat.sh
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-next-version-policy-actions
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_next_version_policy.sh
    LABELS kernel;audit
    TIMEOUT 30)

# A CLI configuration always supplies the prepared replay. A tooling-only
# configuration may opt in by naming one explicitly; the fixture itself is
# host-only and rootfs-free.
if(DEFINED UML_KERNEL_SOURCE_DIR AND
   EXISTS "${UML_KERNEL_SOURCE_DIR}/arch/um/kernel/tlb.c")
    uml_add_tooling_test(
        NAME darwin-init-mm-tlb-policy
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_darwin_init_mm_tlb_policy.py
            --linux-source ${UML_KERNEL_SOURCE_DIR}
        LABELS kernel;audit
        TIMEOUT 30)
endif()
