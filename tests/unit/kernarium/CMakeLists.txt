# SPDX-License-Identifier: GPL-2.0-or-later

# Kernarium's runtime is an iOS framework, but its console input is plain
# POSIX and is exercised here on the host.
add_executable(uml_kernarium_console_input_tests console_input_tests.cpp)
target_include_directories(uml_kernarium_console_input_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime)
uml_add_unit_test(NAME kernarium-console-input
    COMMAND uml_kernarium_console_input_tests
    LABELS kernarium)
