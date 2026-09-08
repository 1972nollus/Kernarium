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
        --kernarium-runtime
        ${PROJECT_SOURCE_DIR}/Kernarium/Runtime/KernariumRuntime.mm
        --kernel-config
        ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/config/common.fragment
    LABELS rootfs;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME rootfs-steam-arm64ec-image-audit
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_steam_arm64ec_image.py
    LABELS rootfs;steam;arm64ec;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME rootfs-starter-image-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_starter_images.py
        --starter-root ${PROJECT_SOURCE_DIR}/tools/rootfs/starter
        --kernel-config
        ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/config/common.fragment
        --android-kernel-config
        ${PROJECT_SOURCE_DIR}/kernel/android-6.18/config/android.fragment
    LABELS rootfs;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME rootfs-wine-arm64ec-pe-audit
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_wine_arm64ec_pe.py
        --verifier
        ${PROJECT_SOURCE_DIR}/tools/rootfs/wine-arm64ec/verify-pe.py
        --builder
        ${PROJECT_SOURCE_DIR}/tools/rootfs/wine-arm64ec/build.sh
    LABELS rootfs;audit
    TIMEOUT 30)
