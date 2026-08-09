# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME rootfs-provisioning
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_provision.py
    LABELS rootfs;provisioning
    TIMEOUT 120)

uml_add_tooling_test(
    NAME rootfs-overlay-launcher-options
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_boot_overlay_systemd.py
        --launcher ${PROJECT_SOURCE_DIR}/scripts/boot-overlay-systemd.sh
    LABELS rootfs;audit
    TIMEOUT 30)
