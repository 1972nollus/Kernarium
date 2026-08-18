# SPDX-License-Identifier: GPL-2.0-or-later

cmake_path(GET CMAKE_CURRENT_BINARY_DIR PARENT_PATH
    _uml_audio_unit_binary_directory)

add_executable(uml_virtio_sound_tests virtio_sound_tests.cpp)
target_link_libraries(uml_virtio_sound_tests PRIVATE UML::AudioDevice)
target_compile_features(uml_virtio_sound_tests PRIVATE cxx_std_20)
target_compile_options(uml_virtio_sound_tests PRIVATE
    -Wall -Wextra -Werror -Wno-deprecated-declarations)
add_dependencies(unit-test-binaries uml_virtio_sound_tests)
uml_add_unit_test(NAME virtio-sound-protocol COMMAND uml_virtio_sound_tests
    WORKING_DIRECTORY "${_uml_audio_unit_binary_directory}"
    LABELS audio)
