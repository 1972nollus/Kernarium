# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(kernarium_print_bootstrap
    ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/print_bootstrap.cpp)
target_include_directories(kernarium_print_bootstrap PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime)
target_compile_features(kernarium_print_bootstrap PRIVATE cxx_std_20)
target_compile_options(kernarium_print_bootstrap PRIVATE
    -Wall -Wextra -Werror)

uml_add_tooling_test(
    NAME kernarium-bootstrap-failure
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_bootstrap_failure.py
        --printer $<TARGET_FILE:kernarium_print_bootstrap>
    LABELS kernarium
    TIMEOUT 60)

uml_add_tooling_test(
    NAME kernarium-ipad-automation-keys
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_ipad_automation_keys.py
    LABELS kernarium
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernarium-ipad-automation-supervisor
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_ipad_automation_supervisor.py
    LABELS kernarium
    TIMEOUT 60)
