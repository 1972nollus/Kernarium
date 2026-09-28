# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_virtio_console_tests virtio_console_tests.cpp)
target_link_libraries(uml_virtio_console_tests PRIVATE UML::ConsoleDevice)
target_compile_features(uml_virtio_console_tests PRIVATE cxx_std_20)
target_compile_options(uml_virtio_console_tests PRIVATE
    -Wall -Wextra -Werror -Wno-deprecated-declarations)
uml_add_unit_test(NAME virtio-console-protocol COMMAND uml_virtio_console_tests
    LABELS console)
