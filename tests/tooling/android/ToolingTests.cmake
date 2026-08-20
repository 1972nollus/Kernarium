# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME android17-builder-lane
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_builder_lane.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME android17-aosp-patch-regenerator
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_aosp_patch_regenerator.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME android17-release-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_release_contract.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 30)
