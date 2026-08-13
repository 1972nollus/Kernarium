# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_virtio_input STATIC
    in_process_service.cpp
    virtio_input_device.cpp)
add_library(UML::VirtioInput ALIAS uml_virtio_input)
uml_declare_architecture_target(uml_virtio_input device)
target_include_directories(uml_virtio_input
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
target_compile_features(uml_virtio_input PUBLIC cxx_std_20)
target_compile_options(uml_virtio_input PRIVATE
    -Wall -Wextra -Werror -Wno-deprecated-declarations)
target_link_libraries(uml_virtio_input PRIVATE uml_gpu_transport)
