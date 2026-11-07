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

# The App Store configuration builds an application that passes this check as
# its last build phase; the fixtures show it finds each problem the script's
# header lists, and leaves the guest kernel image alone.
uml_add_tooling_test(
    NAME andarium-app-store-bundle
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_check_app_store_bundle.py
        --checker ${PROJECT_SOURCE_DIR}/Andarium/scripts/check-app-store-bundle.py
    LABELS andarium;package
    TIMEOUT 240)

# The Acknowledgements resource names a project and its license text for each
# framework the application embeds and each library of its iOS device prefixes.
uml_add_tooling_test(
    NAME andarium-acknowledgements
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_acknowledgements.py
    LABELS andarium
    TIMEOUT 60)

# The screenshot script takes one run at a time into a directory, refuses a
# second run at once and releases the directory however a run ends.
uml_add_tooling_test(
    NAME andarium-capture-screens
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_capture_screens.py
    LABELS andarium
    TIMEOUT 60)
