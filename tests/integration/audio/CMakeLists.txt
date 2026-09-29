# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_apple_audio_playback_tests apple_playback_tests.cpp)
target_link_libraries(uml_apple_audio_playback_tests PRIVATE UML::AudioDevice)
target_compile_options(uml_apple_audio_playback_tests PRIVATE -Wall -Wextra -Werror)
uml_add_integration_test(NAME apple-audio-playback
    COMMAND uml_apple_audio_playback_tests LABELS audio TIMEOUT 20)
