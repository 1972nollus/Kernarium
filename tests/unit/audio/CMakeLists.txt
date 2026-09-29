# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_virtio_sound_tests virtio_sound_tests.cpp)
target_link_libraries(uml_virtio_sound_tests PRIVATE UML::AudioDevice)
target_compile_options(uml_virtio_sound_tests PRIVATE
    -Wno-deprecated-declarations)
uml_add_unit_test(NAME virtio-sound-protocol COMMAND uml_virtio_sound_tests
    LABELS audio)
