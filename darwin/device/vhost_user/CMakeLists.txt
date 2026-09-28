# SPDX-License-Identifier: GPL-2.0-or-later

# The device-neutral vhost-user transport and in-process service scaffold
# that every vhost-user device is built on.
function(uml_configure_vhost_user_component target)
    uml_declare_architecture_target(${target} device)
    target_include_directories(${target} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
endfunction()

add_library(uml_vhost_user_wire STATIC vhost_user_message.cpp)
uml_configure_vhost_user_component(uml_vhost_user_wire)

add_library(uml_vhost_user_transport STATIC
    shared_memory_regions.cpp
    vhost_user_server.cpp)
add_library(UML::VhostUserTransport ALIAS uml_vhost_user_transport)
uml_configure_vhost_user_component(uml_vhost_user_transport)
target_link_libraries(uml_vhost_user_transport PUBLIC
    UML::DeviceSocketPaths
    UML::MemorySharedBacking
    uml_vhost_user_wire)
# AutoreleaseScope calls the Objective-C runtime directly.
target_link_libraries(uml_vhost_user_transport PRIVATE UML::GuestTimerMask objc)
