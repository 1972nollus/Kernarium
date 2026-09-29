# SPDX-License-Identifier: GPL-2.0-or-later

cmake_path(GET CMAKE_CURRENT_BINARY_DIR PARENT_PATH
    _uml_unit_binary_directory)

find_package(Threads REQUIRED)
add_executable(uml_gpu_metal_submit_timing_tests metal_submit_timing_tests.cpp)
target_link_libraries(uml_gpu_metal_submit_timing_tests PRIVATE Threads::Threads)
uml_add_unit_test(
    NAME gpu-metal-submit-timing-collector
    COMMAND uml_gpu_metal_submit_timing_tests
    LABELS gpu;vulkan)

# Guest allocations and the host GPU driver's own share one reserving pool.
if(TARGET uml_gpu_device_memory_pool)
    add_executable(uml_gpu_device_memory_pool_tests
        device_memory_pool_tests.cpp)
    target_link_libraries(uml_gpu_device_memory_pool_tests PRIVATE
        UML::GpuDeviceMemoryPool
        Threads::Threads)
    uml_add_unit_test(
        NAME gpu-device-memory-pool
        COMMAND uml_gpu_device_memory_pool_tests
        LABELS gpu
        TIMEOUT 60)
endif()

# The balloon follows host GPU memory that Metal's allocated size omits.
if(TARGET uml_balloon_device)
    add_executable(uml_gpu_host_graphics_footprint_tests
        host_graphics_footprint_tests.mm)
    target_link_libraries(uml_gpu_host_graphics_footprint_tests PRIVATE
        UML::BalloonDevice
        UML::HostGpuMemory
        "-framework Metal")
    uml_add_unit_test(
        NAME gpu-host-graphics-footprint
        COMMAND uml_gpu_host_graphics_footprint_tests
        LABELS gpu
        TIMEOUT 60)
    set_tests_properties(gpu-host-graphics-footprint PROPERTIES
        SKIP_RETURN_CODE 77)
endif()

if(TARGET uml_gpu_kosmickrisp_capability_probe)
    add_executable(uml_gpu_kosmickrisp_external_image_alias_tests
        kosmickrisp_external_image_alias_tests.cpp)
    target_link_libraries(uml_gpu_kosmickrisp_external_image_alias_tests PRIVATE
        UML::ThirdPartyKosmicKrisp)
    uml_add_unit_test(
        NAME gpu-kosmickrisp-external-image-alias
        COMMAND uml_gpu_kosmickrisp_external_image_alias_tests
        LABELS gpu;vulkan
        TIMEOUT 120)

    add_executable(uml_gpu_kosmickrisp_timestamp_availability_tests
        kosmickrisp_timestamp_availability_tests.cpp)
    target_link_libraries(uml_gpu_kosmickrisp_timestamp_availability_tests PRIVATE
        UML::ThirdPartyKosmicKrisp)
    uml_add_unit_test(
        NAME gpu-kosmickrisp-timestamp-availability
        COMMAND uml_gpu_kosmickrisp_timestamp_availability_tests
        LABELS gpu;vulkan
        TIMEOUT 120)

    add_library(uml_gpu_vulkan_draw_test_support STATIC
        vulkan_draw_test.cpp)
    target_link_libraries(uml_gpu_vulkan_draw_test_support PUBLIC
        UML::ThirdPartyKosmicKrisp)
    target_include_directories(uml_gpu_vulkan_draw_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})
    add_executable(uml_gpu_kosmickrisp_line_multiview_tests
        kosmickrisp_line_multiview_tests.cpp)
    target_link_libraries(uml_gpu_kosmickrisp_line_multiview_tests PRIVATE
        uml_gpu_vulkan_draw_test_support)
    uml_add_unit_test(
        NAME gpu-kosmickrisp-line-multiview
        COMMAND uml_gpu_kosmickrisp_line_multiview_tests
        LABELS gpu;vulkan)

    # Each runs the KosmicKrisp capability probe and checks one capability in
    # its report.
    foreach(capability
            dynamic_logic_op device_address_commands graphics_pipeline_library
            legacy_vertex_attribute rgb32_texel non_seamless_cube
            incremental_present unused_attachments)
        set(target uml_gpu_kosmickrisp_${capability}_capability_tests)
        string(REPLACE "_" "-" test_name "${capability}")
        add_executable(${target}
            kosmickrisp_${capability}_capability_tests.cpp)
        target_link_libraries(${target} PRIVATE
            UML::GpuKosmicKrispCapabilityProbe)
        target_include_directories(${target} PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
        uml_add_unit_test(
            NAME gpu-kosmickrisp-${test_name}-capabilities
            COMMAND ${target}
            LABELS gpu;vulkan)
    endforeach()
    target_link_libraries(uml_gpu_kosmickrisp_rgb32_texel_capability_tests
        PRIVATE UML::ThirdPartyKosmicKrisp)

    add_executable(uml_gpu_kosmickrisp_legacy_vertex_input_draw_tests
        kosmickrisp_legacy_vertex_input_draw_tests.cpp)
    target_link_libraries(uml_gpu_kosmickrisp_legacy_vertex_input_draw_tests PRIVATE
        uml_gpu_vulkan_draw_test_support)
    uml_add_unit_test(
        NAME gpu-kosmickrisp-legacy-vertex-input-draws
        COMMAND uml_gpu_kosmickrisp_legacy_vertex_input_draw_tests
        LABELS gpu;vulkan)
endif()

if(TARGET uml_gpu_vulkan_draw_test_support)
    add_executable(uml_gpu_vulkan_device_address_commands_tests
        vulkan_device_address_commands_tests.cpp)
    target_link_libraries(uml_gpu_vulkan_device_address_commands_tests PRIVATE
        uml_gpu_vulkan_draw_test_support)
    uml_add_unit_test(
        NAME gpu-vulkan-device-address-commands
        COMMAND uml_gpu_vulkan_device_address_commands_tests
        LABELS gpu;vulkan
        TIMEOUT 600)

    add_executable(uml_gpu_kosmickrisp_geometry_invocations_tests
        kosmickrisp_geometry_invocations_tests.cpp)
    target_link_libraries(uml_gpu_kosmickrisp_geometry_invocations_tests PRIVATE
        uml_gpu_vulkan_draw_test_support Threads::Threads)
    uml_add_unit_test(
        NAME gpu-kosmickrisp-geometry-invocations
        COMMAND uml_gpu_kosmickrisp_geometry_invocations_tests
        LABELS gpu;vulkan)

    add_executable(uml_gpu_kosmickrisp_partial_position_tests
        kosmickrisp_partial_position_tests.cpp)
    target_link_libraries(uml_gpu_kosmickrisp_partial_position_tests PRIVATE
        uml_gpu_vulkan_draw_test_support)
    uml_add_unit_test(
        NAME gpu-kosmickrisp-partial-position
        COMMAND uml_gpu_kosmickrisp_partial_position_tests
        LABELS gpu;vulkan)

    add_executable(uml_gpu_kosmickrisp_pipeline_statistics_tests
        kosmickrisp_pipeline_statistics_tests.cpp)
    target_link_libraries(uml_gpu_kosmickrisp_pipeline_statistics_tests PRIVATE
        uml_gpu_vulkan_draw_test_support)
    uml_add_unit_test(
        NAME gpu-kosmickrisp-pipeline-statistics
        COMMAND uml_gpu_kosmickrisp_pipeline_statistics_tests
        LABELS gpu;vulkan
        TIMEOUT 600)

    add_executable(uml_gpu_kosmickrisp_point_clipping_tests
        kosmickrisp_point_clipping_tests.cpp)
    target_link_libraries(uml_gpu_kosmickrisp_point_clipping_tests PRIVATE
        uml_gpu_vulkan_draw_test_support)
    uml_add_unit_test(
        NAME gpu-kosmickrisp-point-clipping
        COMMAND uml_gpu_kosmickrisp_point_clipping_tests
        LABELS gpu;vulkan
        TIMEOUT 600)

    add_executable(uml_gpu_kosmickrisp_tessellation_feedback_tests
        kosmickrisp_tessellation_feedback_tests.cpp)
    target_link_libraries(uml_gpu_kosmickrisp_tessellation_feedback_tests PRIVATE
        uml_gpu_vulkan_draw_test_support)
    uml_add_unit_test(
        NAME gpu-kosmickrisp-tessellation-feedback
        COMMAND uml_gpu_kosmickrisp_tessellation_feedback_tests
        LABELS gpu;vulkan
        TIMEOUT 600)

    add_executable(uml_gpu_kosmickrisp_command_storage_tests
        kosmickrisp_command_storage_tests.cpp)
    target_link_libraries(uml_gpu_kosmickrisp_command_storage_tests PRIVATE
        uml_gpu_vulkan_draw_test_support)
    uml_add_unit_test(
        NAME gpu-kosmickrisp-command-storage
        COMMAND uml_gpu_kosmickrisp_command_storage_tests
        LABELS gpu;vulkan
        TIMEOUT 600)

    add_executable(uml_gpu_kosmickrisp_raster_polygon_tests
        kosmickrisp_raster_polygon_tests.cpp)
    target_link_libraries(uml_gpu_kosmickrisp_raster_polygon_tests PRIVATE
        uml_gpu_vulkan_draw_test_support)
    uml_add_unit_test(
        NAME gpu-kosmickrisp-raster-polygon
        COMMAND uml_gpu_kosmickrisp_raster_polygon_tests
        LABELS gpu;vulkan
        TIMEOUT 600)

    add_executable(uml_gpu_kosmickrisp_raster_line_tests
        kosmickrisp_raster_line_tests.cpp)
    target_link_libraries(uml_gpu_kosmickrisp_raster_line_tests PRIVATE
        uml_gpu_vulkan_draw_test_support Threads::Threads)
    uml_add_unit_test(
        NAME gpu-kosmickrisp-raster-line
        COMMAND uml_gpu_kosmickrisp_raster_line_tests
        LABELS gpu;vulkan
        TIMEOUT 600)

    add_executable(uml_gpu_kosmickrisp_raster_state_tests
        kosmickrisp_raster_state_tests.cpp)
    target_link_libraries(uml_gpu_kosmickrisp_raster_state_tests PRIVATE
        uml_gpu_vulkan_draw_test_support)
    uml_add_unit_test(
        NAME gpu-kosmickrisp-raster-state
        COMMAND uml_gpu_kosmickrisp_raster_state_tests
        LABELS gpu;vulkan
        TIMEOUT 600)

    add_executable(uml_gpu_kosmickrisp_geometry_side_effects_tests
        kosmickrisp_geometry_side_effects_tests.cpp)
    target_link_libraries(uml_gpu_kosmickrisp_geometry_side_effects_tests PRIVATE
        uml_gpu_vulkan_draw_test_support Threads::Threads)
    uml_add_unit_test(
        NAME gpu-kosmickrisp-geometry-side-effects
        COMMAND uml_gpu_kosmickrisp_geometry_side_effects_tests
        LABELS gpu;vulkan
        TIMEOUT 600)
endif()

if(TARGET uml_gpu_service)
    add_executable(uml_gpu_host_gl_provider_tests
        host_gl_provider_lifetime_tests.cpp)
    target_link_libraries(uml_gpu_host_gl_provider_tests PRIVATE
        uml_gpu_renderer
        UML::ThirdPartyVirglrenderer)
    target_include_directories(uml_gpu_host_gl_provider_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    target_compile_options(uml_gpu_host_gl_provider_tests PRIVATE
        -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-host-gl-provider-lifetime
        COMMAND uml_gpu_host_gl_provider_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu)

    # The host GL provider has to be able to alias memory the caller already
    # owns as a sampled texture; the guest scanout import depends on it.
    add_executable(uml_gpu_host_pointer_image_tests
        host_gl_host_pointer_image_tests.cpp)
    target_link_libraries(uml_gpu_host_pointer_image_tests PRIVATE
        uml_gpu_renderer
        UML::ThirdPartyVirglrenderer
        UML::ThirdPartyEpoxy)
    target_include_directories(uml_gpu_host_pointer_image_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    target_compile_options(uml_gpu_host_pointer_image_tests PRIVATE
        -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-host-pointer-image
        COMMAND uml_gpu_host_pointer_image_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu
        TIMEOUT 600)

    # A transfer a context refuses must not leave that context refusing every
    # later transfer: Android's gralloc mapper only ever transfers.
    add_executable(uml_gpu_transfer_context_error_tests
        host_gl_transfer_context_error_tests.cpp)
    target_link_libraries(uml_gpu_transfer_context_error_tests PRIVATE
        uml_gpu_renderer
        UML::ThirdPartyVirglrenderer
        UML::ThirdPartyEpoxy)
    target_include_directories(uml_gpu_transfer_context_error_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    target_compile_options(uml_gpu_transfer_context_error_tests PRIVATE
        -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-transfer-context-error
        COMMAND uml_gpu_transfer_context_error_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu
        TIMEOUT 600)

    # A layout the guest names has to fit the host storage behind it.
    add_executable(uml_gpu_import_bounds_tests
        host_gl_import_bounds_tests.mm)
    target_link_libraries(uml_gpu_import_bounds_tests PRIVATE
        uml_gpu_renderer
        UML::ThirdPartyVirglrenderer
        UML::ThirdPartyEpoxy
        "-framework Foundation"
        "-framework Metal")
    target_include_directories(uml_gpu_import_bounds_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    target_compile_options(uml_gpu_import_bounds_tests PRIVATE
        -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-import-bounds
        COMMAND uml_gpu_import_bounds_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu
        TIMEOUT 600)

    # The native display samples the Metal texture that stores a Virgl
    # scanout's GL texture, so the renderer must name the right one: its size,
    # format, orientation, device, and fenced contents.
    add_executable(uml_gpu_resource_metal_texture_tests
        host_gl_resource_metal_texture_tests.mm)
    target_link_libraries(uml_gpu_resource_metal_texture_tests PRIVATE
        uml_gpu_renderer
        UML::ThirdPartyVirglrenderer
        UML::ThirdPartyEpoxy
        "-framework Foundation"
        "-framework Metal")
    target_include_directories(uml_gpu_resource_metal_texture_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    target_compile_options(uml_gpu_resource_metal_texture_tests PRIVATE
        -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-resource-metal-texture
        COMMAND uml_gpu_resource_metal_texture_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu
        TIMEOUT 600)

    # One thread drives every host GL context, so a read in one context of a
    # texture another context left unsubmitted has to finish without a second
    # thread. Blocking there parks the GPU thread and wedges the whole guest.
    add_executable(uml_gpu_cross_context_readback_tests
        host_gl_cross_context_readback_tests.cpp)
    target_link_libraries(uml_gpu_cross_context_readback_tests PRIVATE
        uml_gpu_renderer
        UML::ThirdPartyVirglrenderer
        UML::ThirdPartyEpoxy)
    target_include_directories(uml_gpu_cross_context_readback_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    target_compile_options(uml_gpu_cross_context_readback_tests PRIVATE
        -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-cross-context-readback
        COMMAND uml_gpu_cross_context_readback_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu
        TIMEOUT 600)

    # Virglrenderer advertises depth formats the guest turns into default
    # framebuffers. A host that substitutes different storage for a sized depth
    # format leaves an OpenGL ES guest unable to name its own depth buffer.
    add_executable(uml_gpu_depth_format_advertisement_tests
        host_gl_depth_format_advertisement_tests.cpp)
    target_link_libraries(uml_gpu_depth_format_advertisement_tests PRIVATE
        uml_gpu_renderer
        UML::ThirdPartyVirglrenderer
        UML::ThirdPartyEpoxy)
    target_include_directories(uml_gpu_depth_format_advertisement_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    target_compile_options(uml_gpu_depth_format_advertisement_tests PRIVATE
        -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-depth-format-advertisement
        COMMAND uml_gpu_depth_format_advertisement_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu
        TIMEOUT 600)

    # GL_VERTICES_SUBMITTED_ARB counts submitted vertices, not assembled ones.
    # Zink does not advertise LINE_LOOP, so Mesa decomposes every line loop
    # into independent lines before the driver sees it, doubling the count
    # unless the driver subtracts the conversion back out.
    add_executable(uml_gpu_line_loop_statistics_tests
        host_gl_line_loop_statistics_tests.cpp)
    target_link_libraries(uml_gpu_line_loop_statistics_tests PRIVATE
        uml_gpu_renderer
        UML::ThirdPartyVirglrenderer
        UML::ThirdPartyEpoxy)
    target_include_directories(uml_gpu_line_loop_statistics_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    target_compile_options(uml_gpu_line_loop_statistics_tests PRIVATE
        -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-line-loop-statistics
        COMMAND uml_gpu_line_loop_statistics_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu
        TIMEOUT 600)

    # The renderer and the same-process render server share one fence table.
    # Exercise that ownership without a device: it is a pure lifetime
    # contract, and losing it faults the GPU thread.
    add_executable(uml_gpu_virgl_fence_table_tests
        virgl_fence_table_ownership_tests.cpp)
    target_link_libraries(uml_gpu_virgl_fence_table_tests PRIVATE
        UML::ThirdPartyVirglrenderer)
    uml_add_unit_test(NAME gpu-virgl-fence-table-ownership
        COMMAND uml_gpu_virgl_fence_table_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu)

    add_executable(uml_gpu_transport_wire_tests
        gpu_allocation_failure.cpp
        vhost_user_message_test_main.cpp
        vhost_user_message_tests.cpp)
    target_link_libraries(uml_gpu_transport_wire_tests PRIVATE
        uml_gpu_transport_wire_stop_wait_test_variant)
    target_include_directories(uml_gpu_transport_wire_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(uml_gpu_transport_wire_tests PRIVATE
        -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-transport-wire-ownership
        COMMAND uml_gpu_transport_wire_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu)

    # Its own executable: the descriptor ceiling is process state, and filling
    # the file table would decide the outcome of anything sharing the process.
    add_executable(uml_gpu_transport_ceiling_tests
        gpu_allocation_failure.cpp
        vhost_user_descriptor_ceiling_test_main.cpp
        vhost_user_descriptor_ceiling_tests.cpp)
    target_link_libraries(uml_gpu_transport_ceiling_tests PRIVATE
        uml_vhost_user_wire)
    target_include_directories(uml_gpu_transport_ceiling_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(uml_gpu_transport_ceiling_tests PRIVATE
        -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-transport-descriptor-ceiling
        COMMAND uml_gpu_transport_ceiling_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu)

    add_executable(uml_gpu_service_fd_tests
        gpu_lifetime_tests.cpp
        in_process_service_testing.cpp
        in_process_service_lifetime_tests.cpp
        vhost_user_protocol_tests.cpp
        vhost_user_server_lifetime_tests.cpp)
    target_link_libraries(uml_gpu_service_fd_tests PRIVATE
        uml_gpu_service_test_variant)
    target_include_directories(uml_gpu_service_fd_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(uml_gpu_service_fd_tests PRIVATE
        -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-service-fd-ownership
        COMMAND uml_gpu_service_fd_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu)

    add_executable(uml_gpu_service_stop_wake_tests
        in_process_service_stop_wake_tests.cpp
        in_process_service_testing.cpp)
    target_include_directories(uml_gpu_service_stop_wake_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${PROJECT_SOURCE_DIR}/darwin/device/vhost_user
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(uml_gpu_service_stop_wake_tests PRIVATE
        -Wno-deprecated-declarations)
    target_link_libraries(uml_gpu_service_stop_wake_tests PRIVATE
        uml_gpu_service_slow_stop_test_variant)
    uml_add_unit_test(NAME gpu-service-stop-wake
        COMMAND uml_gpu_service_stop_wake_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu)

    add_executable(uml_gpu_device_atomicity_tests
        gpu_allocation_failure.cpp
        virtio_gpu_device_test_main.cpp
        virtio_gpu_device_lifetime_tests.cpp)
    target_link_libraries(uml_gpu_device_atomicity_tests PRIVATE
        uml_gpu_protocol_device_test_variant)
    target_include_directories(uml_gpu_device_atomicity_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(uml_gpu_device_atomicity_tests PRIVATE
        -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-device-command-atomicity
        COMMAND uml_gpu_device_atomicity_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu)
endif()

if(TARGET uml_gpu_display)
    # The resize path's objects come from the shipped display library.
    add_executable(uml_gpu_display_resize_tests
        display_resize_tests.cpp)
    target_link_libraries(uml_gpu_display_resize_tests PRIVATE
        uml_gpu_display)
    uml_add_unit_test(NAME gpu-display-resize
        COMMAND uml_gpu_display_resize_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu)

    add_executable(uml_gpu_vnc_lifetime_tests
        vnc_display_lifetime_tests.cpp
        vnc_display_metal_testing.mm
        vnc_display_testing.cpp)
    target_link_libraries(uml_gpu_vnc_lifetime_tests PRIVATE
        uml_gpu_display_test_variant
        "-framework Foundation"
        "-framework Metal")
    target_compile_options(uml_gpu_vnc_lifetime_tests PRIVATE
        -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-vnc-lifetime
        COMMAND uml_gpu_vnc_lifetime_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu)

    # This one links the shipped display library rather than a test variant,
    # so it also answers whether the product was built with the native Metal
    # readback that a Venus scanout has no alternative to.
    if(UML_TARGET_MACOS)
        add_executable(uml_gpu_vnc_product_tests
            vnc_display_product_tests.mm)
        target_link_libraries(uml_gpu_vnc_product_tests PRIVATE
            uml_gpu_display
            "-framework Foundation"
            "-framework Metal")
        target_compile_options(uml_gpu_vnc_product_tests PRIVATE
            -Wno-deprecated-declarations)
        uml_add_unit_test(NAME gpu-vnc-product-readback
            COMMAND uml_gpu_vnc_product_tests
            WORKING_DIRECTORY "${_uml_unit_binary_directory}"
            LABELS gpu)

        # The shipped display must return when the guest finishes while a
        # menu is open. It needs a window server session and skips without
        # one.
        add_executable(uml_gpu_native_display_menu_teardown_tests
            native_display_menu_teardown_tests.mm)
        target_link_libraries(uml_gpu_native_display_menu_teardown_tests PRIVATE
            uml_gpu_display
            "-framework AppKit"
            "-framework CoreGraphics")
        target_include_directories(uml_gpu_native_display_menu_teardown_tests
            PRIVATE ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
        uml_add_unit_test(NAME gpu-native-display-menu-teardown
            COMMAND uml_gpu_native_display_menu_teardown_tests
            WORKING_DIRECTORY "${_uml_unit_binary_directory}"
            LABELS gpu
            TIMEOUT 90)
        set_tests_properties(gpu-native-display-menu-teardown PROPERTIES
            SKIP_RETURN_CODE 77)

        # The native presenter must scale a scanout whose size or rectangle
        # differs from its framebuffer. It draws into an offscreen view and
        # skips on a host without a Metal device or offscreen drawables.
        add_executable(uml_gpu_metal_presentation_tests
            metal_presentation_tests.mm)
        target_link_libraries(uml_gpu_metal_presentation_tests PRIVATE
            uml_gpu_display
            "-framework AppKit"
            "-framework Metal"
            "-framework MetalKit"
            "-framework QuartzCore")
        target_include_directories(uml_gpu_metal_presentation_tests
            PRIVATE ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
        uml_add_unit_test(NAME gpu-native-metal-presentation
            COMMAND uml_gpu_metal_presentation_tests
            WORKING_DIRECTORY "${_uml_unit_binary_directory}"
            LABELS gpu
            TIMEOUT 90)
        set_tests_properties(gpu-native-metal-presentation PROPERTIES
            SKIP_RETURN_CODE 77)
    endif()
endif()

unset(_uml_unit_binary_directory)
