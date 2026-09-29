# SPDX-License-Identifier: GPL-2.0-or-later

function(uml_configure_gpu_component target)
    uml_declare_architecture_target(${target} device)
    target_include_directories(${target} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/device/vhost_user)
endfunction()

if(TARGET uml_third_party_kosmickrisp)
    add_library(uml_gpu_kosmickrisp_capability_probe STATIC
        kosmickrisp_capability_probe.mm)
    add_library(UML::GpuKosmicKrispCapabilityProbe ALIAS
        uml_gpu_kosmickrisp_capability_probe)
    uml_configure_gpu_component(uml_gpu_kosmickrisp_capability_probe)
    target_compile_definitions(uml_gpu_kosmickrisp_capability_probe PRIVATE
        VK_ENABLE_BETA_EXTENSIONS=1)
    target_link_libraries(uml_gpu_kosmickrisp_capability_probe PRIVATE
        UML::ThirdPartyKosmicKrisp
        "-framework Foundation"
        "-framework Metal")
    add_dependencies(uml_gpu_kosmickrisp_capability_probe
        uml_dependency_prefix_verify)
endif()

# Keep the renderer, display, protocol device, transport, and in-process
# service as independently owned units. Their dependency direction follows
# the data path and contains no reverse service callbacks.
add_library(uml_gpu_device_memory_pool STATIC device_memory_pool.cpp)
add_library(UML::GpuDeviceMemoryPool ALIAS uml_gpu_device_memory_pool)
uml_configure_gpu_component(uml_gpu_device_memory_pool)

add_library(uml_gpu_renderer STATIC
    host_gl_provider.cpp
    virgl_renderer.cpp)
uml_configure_gpu_component(uml_gpu_renderer)
target_compile_definitions(uml_gpu_renderer PRIVATE
    VIRGL_RENDERER_UNSTABLE_APIS)
target_link_libraries(uml_gpu_renderer PRIVATE
    UML::GpuDeviceMemoryPool
    UML::ThirdPartyVirglrenderer
    UML::HostGpuMemory)
target_link_libraries(uml_gpu_renderer PRIVATE
    "-framework CoreFoundation"
    "-framework Foundation"
    "-framework Metal"
    "-framework QuartzCore")
add_dependencies(uml_gpu_renderer uml_dependency_prefix_verify)

set(_uml_gpu_display_sources
    display_geometry.cpp
    display_resize_notifier.cpp
    display.cpp
    metal_presentation.mm
    metal_scanout_shader.mm
    native_display_common.cpp
    scanout_framebuffer.cpp)
if(UML_TARGET_MACOS)
    list(APPEND _uml_gpu_display_sources
        metal_readback.mm
        native_display.mm
        vnc_display.cpp)
else()
    list(APPEND _uml_gpu_display_sources native_display_ios.mm)
endif()
add_library(uml_gpu_display STATIC ${_uml_gpu_display_sources})
uml_configure_gpu_component(uml_gpu_display)
target_compile_definitions(uml_gpu_display PRIVATE
    UML_GPU_HAS_VNC=$<BOOL:${UML_TARGET_MACOS}>
    UML_GPU_VNC_STARTUP_TESTING=0)
target_include_directories(uml_gpu_display PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/device/input/vnc)
target_link_libraries(uml_gpu_display PUBLIC UML::VncInput)
target_link_libraries(uml_gpu_display PRIVATE UML::GuestTimerMask)
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

if(UML_BUILD_UNIT_TESTS AND UML_TARGET_MACOS)
    # The VNC display with its testing accessors, for the VNC lifetime tests.
    add_library(uml_gpu_display_test_variant STATIC
        display_geometry.cpp
        metal_readback.mm
        metal_scanout_shader.mm
        scanout_framebuffer.cpp
        vnc_display.cpp)
    uml_configure_gpu_component(uml_gpu_display_test_variant)
    target_compile_definitions(uml_gpu_display_test_variant PUBLIC
        UML_GPU_VNC_STARTUP_TESTING=1)
    target_include_directories(uml_gpu_display_test_variant PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin/device/input/vnc)
    target_link_libraries(uml_gpu_display_test_variant PUBLIC UML::VncInput)
    target_link_libraries(uml_gpu_display_test_variant PRIVATE
        UML::ThirdPartyVncServer
        "-framework Accelerate"
        "-framework Foundation"
        "-framework Metal")
    add_dependencies(uml_gpu_display_test_variant
        uml_dependency_prefix_verify)
endif()

add_library(uml_gpu_protocol_device STATIC virtio_gpu_device.cpp)
uml_configure_gpu_component(uml_gpu_protocol_device)
target_link_libraries(uml_gpu_protocol_device PUBLIC
    uml_gpu_renderer
    uml_gpu_display)

# The Virtio-GPU device on the vhost-user transport, with its display.
add_library(uml_gpu_vhost_user_device STATIC
    deferred_fences.cpp
    gpu_vhost_user_server.cpp)
uml_configure_gpu_component(uml_gpu_vhost_user_device)
target_link_libraries(uml_gpu_vhost_user_device PUBLIC
    UML::VhostUserTransport
    uml_gpu_protocol_device
    UML::VncInput)

add_library(uml_gpu_service STATIC in_process_service.cpp)
uml_configure_gpu_component(uml_gpu_service)
target_compile_definitions(uml_gpu_service PRIVATE
    UML_GPU_STARTUP_TESTING=0)
target_link_libraries(uml_gpu_service PRIVATE uml_gpu_vhost_user_device)

if(UML_BUILD_UNIT_TESTS)
    # Test variants for the GPU service and device tests. Each recompiles the
    # production sources under a test-only policy; the hooks those policies
    # call are implemented beside the tests.
    add_library(uml_gpu_service_test_variant STATIC
        deferred_fences.cpp
        gpu_vhost_user_server.cpp
        in_process_service.cpp)
    uml_configure_gpu_component(uml_gpu_service_test_variant)
    target_compile_definitions(uml_gpu_service_test_variant PRIVATE
        UML_GPU_STARTUP_TESTING=1)
    target_link_libraries(uml_gpu_service_test_variant PUBLIC
        uml_gpu_transport_startup_test_variant
        uml_gpu_protocol_device
        UML::VncInput)

    add_library(uml_gpu_service_slow_stop_test_variant STATIC
        in_process_service.cpp)
    uml_configure_gpu_component(uml_gpu_service_slow_stop_test_variant)
    target_compile_definitions(uml_gpu_service_slow_stop_test_variant PRIVATE
        UML_GPU_STARTUP_TESTING=0
        UML_GPU_SERVICE_TRANSPORT_TESTING=1)
    target_link_libraries(uml_gpu_service_slow_stop_test_variant PRIVATE
        uml_gpu_transport_slow_stop_test_variant
        uml_gpu_protocol_device)

    # The device with its command-atomicity probes, and the fence queue it
    # completes into.
    add_library(uml_gpu_protocol_device_test_variant STATIC
        deferred_fences.cpp
        virtio_gpu_device.cpp)
    uml_configure_gpu_component(uml_gpu_protocol_device_test_variant)
    target_compile_definitions(uml_gpu_protocol_device_test_variant PUBLIC
        UML_GPU_DEVICE_TESTING=1)
endif()

add_library(uml_gpu_device INTERFACE)
add_library(UML::GpuDevice ALIAS uml_gpu_device)
uml_declare_architecture_target(uml_gpu_device device)
target_link_libraries(uml_gpu_device INTERFACE
    uml_gpu_service)
