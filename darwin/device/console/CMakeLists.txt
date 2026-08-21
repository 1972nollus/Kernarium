# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_console_device STATIC
    in_process_service.cpp
    unix_socket_channel.cpp
    virtio_console_device.cpp)
add_library(UML::ConsoleDevice ALIAS uml_console_device)
uml_declare_architecture_target(uml_console_device device)
target_include_directories(uml_console_device
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
target_compile_features(uml_console_device PUBLIC cxx_std_20)
target_compile_options(uml_console_device PRIVATE
    -Wall -Wextra -Werror -Wno-deprecated-declarations)
target_link_libraries(uml_console_device PRIVATE uml_gpu_transport)
