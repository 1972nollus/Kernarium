# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_guest_file_sync_tests guest_file_sync_tests.cpp)
target_link_libraries(uml_guest_file_sync_tests PRIVATE UML::GuestFiles)
target_compile_features(uml_guest_file_sync_tests PRIVATE cxx_std_20)
target_compile_options(uml_guest_file_sync_tests PRIVATE -Wall -Wextra -Werror)
add_dependencies(unit-test-binaries uml_guest_file_sync_tests)
uml_add_unit_test(NAME platform-guest-file-sync
    COMMAND uml_guest_file_sync_tests)

add_executable(uml_video_decoder_tests video_decoder_tests.cpp)
target_link_libraries(uml_video_decoder_tests PRIVATE UML::VideoDecoder)
target_compile_features(uml_video_decoder_tests PRIVATE cxx_std_20)
target_compile_options(uml_video_decoder_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(uml_video_decoder_tests PRIVATE
    UML_VIDEO_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/support/video")
add_dependencies(unit-test-binaries uml_video_decoder_tests)
uml_add_unit_test(NAME platform-video-decoder COMMAND uml_video_decoder_tests
    LABELS video TIMEOUT 120)
