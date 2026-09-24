# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_virtio_balloon_tests virtio_balloon_tests.cpp)
target_link_libraries(uml_virtio_balloon_tests PRIVATE UML::BalloonDevice)
target_compile_features(uml_virtio_balloon_tests PRIVATE cxx_std_20)
target_compile_options(uml_virtio_balloon_tests PRIVATE
    -Wall -Wextra -Werror)
add_dependencies(unit-test-binaries uml_virtio_balloon_tests)
uml_add_unit_test(NAME virtio-balloon-protocol COMMAND uml_virtio_balloon_tests
    LABELS "unit;device;balloon")
