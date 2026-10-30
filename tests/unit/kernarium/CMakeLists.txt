# SPDX-License-Identifier: GPL-2.0-or-later

# Kernarium's runtime is an iOS framework, but its console input, the file
# handling of the automation boot payload and the key wake policy are plain
# POSIX and C++ and are exercised here on the host.
add_executable(uml_kernarium_console_input_tests console_input_tests.cpp)
target_include_directories(uml_kernarium_console_input_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime)
uml_add_unit_test(NAME kernarium-console-input
    COMMAND uml_kernarium_console_input_tests
    LABELS kernarium)

add_executable(uml_kernarium_payload_file_tests payload_file_tests.cpp)
target_include_directories(uml_kernarium_payload_file_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime)
uml_add_unit_test(NAME kernarium-payload-file
    COMMAND uml_kernarium_payload_file_tests
    LABELS kernarium)

# Which key events wake a guest that suspended itself (a touch never does).
add_executable(uml_kernarium_key_wake_tests key_wake_tests.cpp)
target_include_directories(uml_kernarium_key_wake_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime)
uml_add_unit_test(NAME kernarium-key-wake
    COMMAND uml_kernarium_key_wake_tests
    LABELS kernarium)
