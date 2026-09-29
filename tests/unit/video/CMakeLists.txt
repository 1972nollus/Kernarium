# SPDX-License-Identifier: GPL-2.0-or-later
add_executable(uml_virtio_video_tests virtio_video_tests.cpp)
target_link_libraries(uml_virtio_video_tests PRIVATE UML::VideoDevice)
uml_add_unit_test(NAME virtio-video-protocol COMMAND uml_virtio_video_tests LABELS video)
