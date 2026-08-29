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
add_library(uml_gpu_renderer STATIC
    host_gl_provider.cpp
    virgl_renderer.cpp)
uml_configure_gpu_component(uml_gpu_renderer)
target_compile_definitions(uml_gpu_renderer PRIVATE
    VIRGL_RENDERER_UNSTABLE_APIS)
target_link_libraries(uml_gpu_renderer PRIVATE UML::ThirdPartyVirglrenderer)
target_link_libraries(uml_gpu_renderer PRIVATE
    "-framework CoreFoundation"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework Foundation"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework VideoToolbox")
add_dependencies(uml_gpu_renderer uml_dependency_prefix_verify)

set(_uml_gpu_display_sources
    display.cpp
    metal_presentation.mm
    scanout_framebuffer.cpp)
if(UML_TARGET_MACOS)
    list(APPEND _uml_gpu_display_sources
        native_display.mm
        vnc_display.cpp)
else()
    list(APPEND _uml_gpu_display_sources native_display_ios.mm)
endif()
add_library(uml_gpu_display STATIC ${_uml_gpu_display_sources})
uml_configure_gpu_component(uml_gpu_display)
set_target_properties(uml_gpu_display PROPERTIES OBJCXX_STANDARD 20)
target_compile_options(uml_gpu_display PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_compile_definitions(uml_gpu_display PRIVATE
    UML_GPU_HAS_VNC=$<BOOL:${UML_TARGET_MACOS}>
    UML_GPU_VNC_STARTUP_TESTING=0)
target_include_directories(uml_gpu_display PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/device/input/vnc)
target_link_libraries(uml_gpu_display PUBLIC UML::VncInput)
if(UML_TARGET_MACOS)
    target_link_libraries(uml_gpu_display PRIVATE
        UML::ThirdPartyVncServer
        "-framework Accelerate"
        "-framework AppKit"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore")
else()
    target_link_libraries(uml_gpu_display PRIVATE
        "-framework Accelerate"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
        "-framework UIKit")
endif()
add_dependencies(uml_gpu_display uml_dependency_prefix_verify)

add_library(uml_gpu_protocol_device STATIC virtio_gpu_device.cpp)
uml_configure_gpu_component(uml_gpu_protocol_device)
target_link_libraries(uml_gpu_protocol_device PUBLIC
    uml_gpu_renderer
    uml_gpu_display)

add_library(uml_gpu_transport_wire STATIC vhost_user_message.cpp)
uml_configure_gpu_component(uml_gpu_transport_wire)

add_library(uml_gpu_transport STATIC vhost_user_server.cpp)
uml_configure_gpu_component(uml_gpu_transport)
target_compile_definitions(uml_gpu_transport PRIVATE
    UML_RUNTIME_PROFILING=$<BOOL:${UML_ENABLE_RUNTIME_PROFILING}>)
target_link_libraries(uml_gpu_transport PUBLIC
    UML::DeviceSocketPaths
    UML::MemorySharedBacking
    uml_gpu_transport_wire
    uml_gpu_protocol_device
    UML::VncInput)

add_library(uml_gpu_service STATIC in_process_service.cpp)
uml_configure_gpu_component(uml_gpu_service)
target_compile_definitions(uml_gpu_service PRIVATE
    UML_GPU_STARTUP_TESTING=0)
target_link_libraries(uml_gpu_service PRIVATE uml_gpu_transport)

add_library(uml_gpu_device INTERFACE)
add_library(UML::GpuDevice ALIAS uml_gpu_device)
uml_declare_architecture_target(uml_gpu_device device)
target_link_libraries(uml_gpu_device INTERFACE
    uml_gpu_service)
