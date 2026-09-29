# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_balloon_device STATIC
    in_process_service.cpp
    virtio_balloon_device.cpp)
add_library(UML::BalloonDevice ALIAS uml_balloon_device)
uml_declare_architecture_target(uml_balloon_device device)
target_include_directories(uml_balloon_device
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/device/vhost_user)
target_link_libraries(uml_balloon_device PRIVATE
    UML::VhostUserTransport
    UML::MemorySharedBacking
    UML::HostGpuMemory)
