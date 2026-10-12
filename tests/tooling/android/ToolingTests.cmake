# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME lineage24-builder-lane
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_builder_lane.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME lineage24-aosp-patch-regenerator
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_aosp_patch_regenerator.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME lineage24-routed-workspaces
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_routed_workspaces.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME lineage24-mesa-venus-source-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_mesa_venus_source_contract.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME lineage24-fdroid-source
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_fdroid_source.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME lineage24-release-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_release_contract.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME android17-macos-launcher-options
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_boot_android.py
    LABELS android
    TIMEOUT 30)
