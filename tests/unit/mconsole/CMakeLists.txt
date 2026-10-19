# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_guest_console_tests guest_console_tests.cpp)
target_link_libraries(uml_guest_console_tests PRIVATE UML::DeviceGuestConsole)
uml_add_unit_test(NAME guest-console-suspend-timeout
    COMMAND uml_guest_console_tests suspend-timeout
    LABELS console)
# A guest that reports a suspend it gave up, even seconds after the request,
# is stopped instead.
uml_add_unit_test(NAME guest-console-suspend-failure
    COMMAND uml_guest_console_tests suspend-failure
    LABELS console)
uml_add_unit_test(NAME guest-console-location
    COMMAND uml_guest_console_tests location
    LABELS console)
uml_add_unit_test(NAME guest-console-run
    COMMAND uml_guest_console_tests run
    LABELS console)
uml_add_unit_test(NAME guest-console-wake
    COMMAND uml_guest_console_tests wake
    LABELS console)
uml_add_unit_test(NAME guest-console-trim
    COMMAND uml_guest_console_tests trim
    LABELS console)
