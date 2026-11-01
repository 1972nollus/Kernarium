# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME andarium-stage-system-image
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_stage_system_image.py
    LABELS andarium
    TIMEOUT 120)

uml_add_tooling_test(
    NAME andarium-device-support
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_andarium_support.py
    LABELS andarium;android
    TIMEOUT 30)

uml_add_tooling_test(
    NAME andarium-android-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_android_runner.py
    LABELS andarium;android
    TIMEOUT 90)
