# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_socket_path_tests socket_path_tests.cpp)
target_link_libraries(uml_socket_path_tests PRIVATE UML::DeviceSocketPaths)
target_compile_features(uml_socket_path_tests PRIVATE cxx_std_20)
target_compile_options(uml_socket_path_tests PRIVATE -Wall -Wextra -Werror)
add_dependencies(unit-test-binaries uml_socket_path_tests)
uml_add_unit_test(NAME device-socket-path-lifetime
    COMMAND uml_socket_path_tests
    LABELS console)
