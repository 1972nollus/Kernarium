# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME gpu-guest-rootfs-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_run_gpu_guest.py
    LABELS gpu
    TIMEOUT 120)

uml_add_tooling_test(
    NAME vk-gl-cts-campaign-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_run_vk_gl_cts.py
    LABELS gpu;cts;rootfs
    TIMEOUT 120)

uml_add_tooling_test(
    NAME kde-vnc-launcher-options
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_boot_kde_vnc.py
    LABELS gpu;vnc
    TIMEOUT 120)

uml_add_tooling_test(
    NAME chromium-webgl-aquarium-metrics
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_run_chromium_webgl.py
    LABELS gpu;webgl
    TIMEOUT 120)
