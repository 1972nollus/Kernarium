# SPDX-License-Identifier: GPL-2.0-or-later

# The mute clock is a header, so its test needs no audio device.
add_executable(uml_audio_mute_clock_tests mute_clock_tests.cpp)
target_include_directories(uml_audio_mute_clock_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/device/audio)
uml_add_unit_test(NAME audio-mute-clock COMMAND uml_audio_mute_clock_tests
    LABELS audio)

if(TARGET uml_audio_device)
    add_executable(uml_virtio_sound_tests virtio_sound_tests.cpp)
    target_link_libraries(uml_virtio_sound_tests PRIVATE UML::AudioDevice)
    uml_add_unit_test(NAME virtio-sound-protocol COMMAND uml_virtio_sound_tests
        LABELS audio)
endif()
