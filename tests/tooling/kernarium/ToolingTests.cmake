# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME kernarium-ipad-automation-keys
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_ipad_automation_keys.py
    LABELS kernarium
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernarium-ipad-automation-gestures
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_ipad_automation_gestures.py
    LABELS kernarium
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernarium-ipad-automation-supervisor
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_ipad_automation_supervisor.py
    LABELS kernarium
    TIMEOUT 60)

if(TARGET uml-initramfs)
    uml_add_tooling_test(
        NAME kernarium-automation-payload
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_automation_payload.py
            --tool $<TARGET_FILE:uml-initramfs>
        LABELS kernarium;boot
        TIMEOUT 60)
endif()
