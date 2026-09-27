# SPDX-License-Identifier: GPL-2.0-or-later
add_executable(uml_virtio_video_tests virtio_video_tests.cpp)
target_link_libraries(uml_virtio_video_tests PRIVATE UML::VideoDevice)
target_compile_features(uml_virtio_video_tests PRIVATE cxx_std_20)
target_compile_options(uml_virtio_video_tests PRIVATE -Wall -Wextra -Werror)
uml_add_unit_test(NAME virtio-video-protocol COMMAND uml_virtio_video_tests LABELS video)
