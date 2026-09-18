# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_test_scratch_directory_tests scratch_directory_tests.cpp)
target_compile_features(uml_test_scratch_directory_tests PRIVATE cxx_std_20)
target_compile_options(uml_test_scratch_directory_tests PRIVATE
    -Wall -Wextra -Werror)
add_dependencies(unit-test-binaries uml_test_scratch_directory_tests)
uml_add_unit_test(NAME unit-test-scratch-directory
    COMMAND uml_test_scratch_directory_tests)
