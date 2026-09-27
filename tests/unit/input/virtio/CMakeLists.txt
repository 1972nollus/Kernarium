# SPDX-License-Identifier: GPL-2.0-or-later

cmake_path(GET CMAKE_CURRENT_BINARY_DIR PARENT_PATH
    _uml_input_binary_directory)
cmake_path(GET _uml_input_binary_directory PARENT_PATH
    _uml_unit_binary_directory)

add_executable(uml_virtio_input_tests virtio_input_tests.cpp)
target_link_libraries(uml_virtio_input_tests PRIVATE UML::VirtioInput)
target_compile_features(uml_virtio_input_tests PRIVATE cxx_std_20)
target_compile_options(uml_virtio_input_tests PRIVATE
    -Wall -Wextra -Werror -Wno-deprecated-declarations)
uml_add_unit_test(NAME virtio-input-protocol COMMAND uml_virtio_input_tests
    WORKING_DIRECTORY "${_uml_unit_binary_directory}"
    LABELS input)
