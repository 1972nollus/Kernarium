# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_virtio_sound_tests virtio_sound_tests.cpp)
target_include_directories(uml_virtio_sound_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_virtio_sound_tests PRIVATE UML::AudioDevice)
uml_add_unit_test(NAME virtio-sound-protocol COMMAND uml_virtio_sound_tests
    LABELS audio)
