# SPDX-License-Identifier: GPL-2.0-or-later

function(uml_configure_gpu_component target)
    uml_declare_architecture_target(${target} device)
    target_include_directories(${target} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
endfunction()

# Keep the renderer, display, protocol device, transport, and in-process
# service as independently owned units. Their dependency direction follows
# the data path and contains no reverse service callbacks.
add_library(uml_gpu_renderer STATIC virgl_renderer.cpp)
uml_configure_gpu_component(uml_gpu_renderer)
target_link_libraries(uml_gpu_renderer PUBLIC
    UML::ThirdPartyVirglrenderer
    "-framework AppKit"
    "-framework CoreFoundation"
    "-framework CoreGraphics"
    "-framework Foundation"
    "-framework IOKit"
    "-framework IOSurface"
    "-framework Metal"
    "-framework OpenGL"
    "-framework QuartzCore")
add_dependencies(uml_gpu_renderer uml_dependency_prefix_verify)

add_library(uml_gpu_display STATIC vnc_display.cpp)
uml_configure_gpu_component(uml_gpu_display)
target_include_directories(uml_gpu_display PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/device/input/vnc)
target_link_libraries(uml_gpu_display PUBLIC
    UML::VncInput
    UML::ThirdPartyVncServer
    "-framework Accelerate")
add_dependencies(uml_gpu_display uml_dependency_prefix_verify)

add_library(uml_gpu_protocol_device STATIC virtio_gpu_device.cpp)
uml_configure_gpu_component(uml_gpu_protocol_device)
target_link_libraries(uml_gpu_protocol_device PUBLIC
    uml_gpu_renderer
    uml_gpu_display)

add_library(uml_gpu_transport STATIC vhost_user_server.cpp)
uml_configure_gpu_component(uml_gpu_transport)
target_link_libraries(uml_gpu_transport PUBLIC
    uml_gpu_protocol_device
    UML::VncInput)

add_library(uml_gpu_service STATIC in_process_service.cpp)
uml_configure_gpu_component(uml_gpu_service)
target_link_libraries(uml_gpu_service PUBLIC uml_gpu_transport)

add_library(uml_gpu_device INTERFACE)
add_library(UML::GpuDevice ALIAS uml_gpu_device)
uml_declare_architecture_target(uml_gpu_device device)
target_link_libraries(uml_gpu_device INTERFACE
    uml_gpu_service
    UML::VncInput)

# This executable is a host-only protocol/VNC test fixture. Production
# emulation links UML::GpuDevice into the emulator process.
add_executable(uml-virtio-gpu main.cpp)
uml_declare_architecture_target(uml-virtio-gpu device)
target_link_libraries(uml-virtio-gpu PRIVATE UML::GpuDevice)
target_compile_options(uml-virtio-gpu PRIVATE
    -Wall -Wextra -Werror -Wno-deprecated-declarations)
set_target_properties(uml-virtio-gpu PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
