# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_guest_file_sync_tests guest_file_sync_tests.cpp)
target_link_libraries(uml_guest_file_sync_tests PRIVATE UML::GuestFiles)
target_compile_features(uml_guest_file_sync_tests PRIVATE cxx_std_20)
target_compile_options(uml_guest_file_sync_tests PRIVATE -Wall -Wextra -Werror)
add_dependencies(unit-test-binaries uml_guest_file_sync_tests)
uml_add_unit_test(NAME platform-guest-file-sync
    COMMAND uml_guest_file_sync_tests)
