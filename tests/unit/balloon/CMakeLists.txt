# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_virtio_balloon_tests virtio_balloon_tests.cpp)
target_link_libraries(uml_virtio_balloon_tests PRIVATE UML::BalloonDevice)
uml_add_unit_test(NAME virtio-balloon-protocol COMMAND uml_virtio_balloon_tests
    LABELS "device;balloon")
