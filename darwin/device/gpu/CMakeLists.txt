# SPDX-License-Identifier: GPL-2.0-or-later

function(uml_configure_gpu_component target)
    uml_declare_architecture_target(${target} device)
    target_include_directories(${target} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
endfunction()

if(TARGET uml_third_party_kosmickrisp)
    add_library(uml_gpu_kosmickrisp_capability_probe STATIC
        kosmickrisp_capability_probe.mm)
    add_library(UML::GpuKosmicKrispCapabilityProbe ALIAS
        uml_gpu_kosmickrisp_capability_probe)
    uml_configure_gpu_component(uml_gpu_kosmickrisp_capability_probe)
    set_target_properties(uml_gpu_kosmickrisp_capability_probe PROPERTIES
        OBJCXX_STANDARD 20)
    target_compile_definitions(uml_gpu_kosmickrisp_capability_probe PRIVATE
        VK_ENABLE_BETA_EXTENSIONS=1)
    target_compile_options(uml_gpu_kosmickrisp_capability_probe PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(uml_gpu_kosmickrisp_capability_probe PRIVATE
        UML::ThirdPartyKosmicKrisp
        "-framework Foundation"
        "-framework Metal")
    add_dependencies(uml_gpu_kosmickrisp_capability_probe
        uml_dependency_prefix_verify)

    add_executable(uml-kosmickrisp-capability-probe
        kosmickrisp_capability_probe_main.cpp)
    uml_declare_architecture_target(
        uml-kosmickrisp-capability-probe device)
    target_compile_features(uml-kosmickrisp-capability-probe PRIVATE
        cxx_std_20)
    target_compile_options(uml-kosmickrisp-capability-probe PRIVATE
        -Wall -Wextra -Werror)
    target_link_libraries(uml-kosmickrisp-capability-probe PRIVATE
        UML::GpuKosmicKrispCapabilityProbe)
    add_dependencies(uml-kosmickrisp-capability-probe
        uml_dependency_prefix_verify)
    set_property(TARGET uml-kosmickrisp-capability-probe APPEND PROPERTY
        LINK_DEPENDS
        ${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}/KosmicKrisp
        ${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}/Info.plist)
    set_target_properties(uml-kosmickrisp-capability-probe PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/Frameworks"
        INSTALL_RPATH_USE_LINK_PATH FALSE)
    set_target_properties(uml-kosmickrisp-capability-probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY
            ${PROJECT_BINARY_DIR}/probes/kosmickrisp)
    set(_uml_kosmickrisp_probe_frameworks
        $<TARGET_FILE_DIR:uml-kosmickrisp-capability-probe>/Frameworks)
    add_custom_command(TARGET uml-kosmickrisp-capability-probe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove_directory
            ${_uml_kosmickrisp_probe_frameworks}/KosmicKrisp.framework
        COMMAND ${CMAKE_COMMAND} -E make_directory
            ${_uml_kosmickrisp_probe_frameworks}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}
            ${_uml_kosmickrisp_probe_frameworks}/KosmicKrisp.framework
        COMMENT "Staging the maintained KosmicKrisp probe framework"
        VERBATIM)
endif()

# Keep the renderer, display, protocol device, transport, and in-process
# service as independently owned units. Their dependency direction follows
# the data path and contains no reverse service callbacks.
add_library(uml_gpu_renderer STATIC
    host_gl_provider.cpp
    virgl_renderer.cpp)
uml_configure_gpu_component(uml_gpu_renderer)
target_compile_definitions(uml_gpu_renderer PRIVATE
    VIRGL_RENDERER_UNSTABLE_APIS
    UML_GPU_VULKAN_EXPERIMENT=$<BOOL:${_uml_apple_gpu_vulkan}>
    UML_GPU_HOST_GL_PROVIDER_MESA=$<BOOL:${_uml_apple_gl_provider_is_mesa}>)
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

if(TARGET uml_third_party_kosmickrisp)
    add_executable(uml-venus-backend-probe
        venus_backend_probe_main.cpp)
    uml_declare_architecture_target(uml-venus-backend-probe device)
    target_compile_features(uml-venus-backend-probe PRIVATE cxx_std_20)
    target_compile_options(uml-venus-backend-probe PRIVATE
        -Wall -Wextra -Werror)
    # The renderer archive carries the VideoToolbox decode backend for both
    # Apple GPU products, so this direct consumer needs the same frameworks.
    target_link_libraries(uml-venus-backend-probe PRIVATE
        UML::ThirdPartyVirglrenderer
        UML::GpuKosmicKrispCapabilityProbe
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Foundation"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework VideoToolbox")
    add_dependencies(uml-venus-backend-probe
        uml_dependency_prefix_verify)
    set_property(TARGET uml-venus-backend-probe APPEND PROPERTY
        LINK_DEPENDS
        ${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}/KosmicKrisp
        ${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}/Info.plist)
    set_target_properties(uml-venus-backend-probe PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/Frameworks"
        INSTALL_RPATH_USE_LINK_PATH FALSE)
    set_target_properties(uml-venus-backend-probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY
            ${PROJECT_BINARY_DIR}/probes/venus)
    set(_uml_venus_probe_frameworks
        $<TARGET_FILE_DIR:uml-venus-backend-probe>/Frameworks)
    set(_uml_venus_probe_gl_stage_commands)
    foreach(_uml_gl_framework IN LISTS UML_APPLE_GPU_GL_FRAMEWORK_NAMES)
        list(APPEND _uml_venus_probe_gl_stage_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/${_uml_gl_framework}.framework
                ${_uml_venus_probe_frameworks}/${_uml_gl_framework}.framework)
    endforeach()
    add_custom_command(TARGET uml-venus-backend-probe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove_directory
            ${_uml_venus_probe_frameworks}
        COMMAND ${CMAKE_COMMAND} -E make_directory
            ${_uml_venus_probe_frameworks}
        ${_uml_venus_probe_gl_stage_commands}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}
            ${_uml_venus_probe_frameworks}/KosmicKrisp.framework
        COMMENT "Staging the maintained Venus backend probe frameworks"
        VERBATIM)
endif()

set(_uml_gpu_display_sources
    display_geometry.cpp
    display_resize_notifier.cpp
    display.cpp
    metal_presentation.mm
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
    UML_RUNTIME_PROFILING=$<BOOL:${UML_ENABLE_RUNTIME_PROFILING}>
    UML_GPU_VULKAN_EXPERIMENT=$<BOOL:${_uml_apple_gpu_vulkan}>)
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
