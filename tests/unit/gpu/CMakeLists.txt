# SPDX-License-Identifier: GPL-2.0-or-later

cmake_path(GET CMAKE_CURRENT_BINARY_DIR PARENT_PATH
    _uml_unit_binary_directory)

find_package(Threads REQUIRED)
set(_uml_gpu_device_directory ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
set(_uml_host_gl_libraries
    uml_gpu_renderer
    UML::ThirdPartyVirglrenderer
    UML::ThirdPartyEpoxy)

# Every GPU unit test process places Metal's shader cache in its scratch
# directory instead of opening the host's shared one.
add_library(uml_gpu_test_metal_shader_cache OBJECT
    metal_shader_cache_isolation.cpp)
target_link_libraries(uml_gpu_test_metal_shader_cache PUBLIC
    "-framework CoreFoundation")

# uml_add_gpu_unit_test(<test> <target> <source>... [LIBRARIES <library>...]
#     [INCLUDES <directory>...] [VULKAN] [UNIT_DIRECTORY] [TIMEOUT <seconds>])
#
# Builds <target> from the sources and registers it as the unit test <test>,
# labelled gpu, and vulkan for VULKAN. UNIT_DIRECTORY runs it in the unit-test
# binary directory. The target takes its own Metal shader cache. The unit-gpu
# test preset selects every test named gpu-*, so <test> must be.
function(uml_add_gpu_unit_test TEST TARGET)
    cmake_parse_arguments(PARSE_ARGV 2 _gpu_test
        "VULKAN;UNIT_DIRECTORY" "TIMEOUT" "LIBRARIES;INCLUDES")
    if(NOT "${TEST}" MATCHES "^gpu-")
        message(FATAL_ERROR
            "${TEST}: a GPU unit test is named gpu-*, which is how the "
            "unit-gpu test preset selects it")
    endif()
    add_executable(${TARGET} ${_gpu_test_UNPARSED_ARGUMENTS})
    target_link_libraries(${TARGET} PRIVATE ${_gpu_test_LIBRARIES}
        uml_gpu_test_metal_shader_cache)
    if(_gpu_test_INCLUDES)
        target_include_directories(${TARGET} PRIVATE ${_gpu_test_INCLUDES})
    endif()
    set(_options LABELS gpu)
    if(_gpu_test_VULKAN)
        list(APPEND _options vulkan)
    endif()
    if(_gpu_test_UNIT_DIRECTORY)
        list(APPEND _options WORKING_DIRECTORY "${_uml_unit_binary_directory}")
    endif()
    if(_gpu_test_TIMEOUT)
        list(APPEND _options TIMEOUT ${_gpu_test_TIMEOUT})
    endif()
    uml_add_unit_test(NAME ${TEST} COMMAND ${TARGET} ${_options})
endfunction()

uml_add_gpu_unit_test(gpu-metal-submit-timing-collector
    uml_gpu_metal_submit_timing_tests metal_submit_timing_tests.cpp
    LIBRARIES Threads::Threads
    VULKAN)

# The emulator bounds its own Metal shader cache by its index; the test needs
# no Metal device.
if(TARGET uml_gpu_metal_shader_cache)
    add_executable(uml_gpu_metal_shader_cache_tests
        metal_shader_cache_tests.cpp)
    target_link_libraries(uml_gpu_metal_shader_cache_tests PRIVATE
        UML::GpuMetalShaderCache)
    uml_add_unit_test(NAME gpu-metal-shader-cache
        COMMAND uml_gpu_metal_shader_cache_tests
        LABELS gpu)
endif()

# Guest allocations and the host GPU driver's own share one reserving pool.
if(TARGET uml_gpu_device_memory_pool)
    uml_add_gpu_unit_test(gpu-device-memory-pool
        uml_gpu_device_memory_pool_tests device_memory_pool_tests.cpp
        LIBRARIES UML::GpuDeviceMemoryPool Threads::Threads
        TIMEOUT 60)
endif()

# The balloon follows host GPU memory that Metal's allocated size omits.
uml_add_gpu_unit_test(gpu-host-graphics-footprint
    uml_gpu_host_graphics_footprint_tests host_graphics_footprint_tests.mm
    LIBRARIES UML::BalloonDevice UML::HostGpuMemory "-framework Metal"
    TIMEOUT 60)
set_tests_properties(gpu-host-graphics-footprint PROPERTIES
    SKIP_RETURN_CODE 77)

if(TARGET uml_third_party_kosmickrisp)
    uml_add_gpu_unit_test(gpu-kosmickrisp-external-image-alias
        uml_gpu_kosmickrisp_external_image_alias_tests
        kosmickrisp_external_image_alias_tests.cpp
        LIBRARIES UML::ThirdPartyKosmicKrisp
        VULKAN TIMEOUT 120)
    uml_add_gpu_unit_test(gpu-kosmickrisp-timestamp-availability
        uml_gpu_kosmickrisp_timestamp_availability_tests
        kosmickrisp_timestamp_availability_tests.cpp
        LIBRARIES UML::ThirdPartyKosmicKrisp
        VULKAN TIMEOUT 120)

    add_library(uml_gpu_vulkan_draw_test_support STATIC
        vulkan_draw_test.cpp)
    target_link_libraries(uml_gpu_vulkan_draw_test_support PUBLIC
        UML::ThirdPartyKosmicKrisp)
    target_include_directories(uml_gpu_vulkan_draw_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})
    uml_add_gpu_unit_test(gpu-kosmickrisp-line-multiview
        uml_gpu_kosmickrisp_line_multiview_tests
        kosmickrisp_line_multiview_tests.cpp
        LIBRARIES uml_gpu_vulkan_draw_test_support
        VULKAN)

    # The probe writes a JSON report of what KosmicKrisp advertises and what
    # its verifiers measure. Each capability test runs it and checks one
    # capability in the report.
    add_library(uml_gpu_kosmickrisp_capability_probe STATIC
        kosmickrisp_capability_probe.mm)
    target_compile_definitions(uml_gpu_kosmickrisp_capability_probe PRIVATE
        VK_ENABLE_BETA_EXTENSIONS=1)
    target_link_libraries(uml_gpu_kosmickrisp_capability_probe PRIVATE
        UML::ThirdPartyKosmicKrisp "-framework Foundation")
    add_dependencies(uml_gpu_kosmickrisp_capability_probe
        uml_dependency_prefix_verify)
    foreach(capability
            dynamic_logic_op device_address_commands graphics_pipeline_library
            legacy_vertex_attribute rgb32_texel non_seamless_cube
            incremental_present unused_attachments)
        string(REPLACE "_" "-" test_name "${capability}")
        uml_add_gpu_unit_test(gpu-kosmickrisp-${test_name}-capabilities
            uml_gpu_kosmickrisp_${capability}_capability_tests
            kosmickrisp_${capability}_capability_tests.cpp
            LIBRARIES uml_gpu_kosmickrisp_capability_probe
            VULKAN)
    endforeach()
    target_link_libraries(uml_gpu_kosmickrisp_rgb32_texel_capability_tests
        PRIVATE UML::ThirdPartyKosmicKrisp)

    uml_add_gpu_unit_test(gpu-kosmickrisp-legacy-vertex-input-draws
        uml_gpu_kosmickrisp_legacy_vertex_input_draw_tests
        kosmickrisp_legacy_vertex_input_draw_tests.cpp
        LIBRARIES uml_gpu_vulkan_draw_test_support
        VULKAN)
endif()

if(TARGET uml_gpu_vulkan_draw_test_support)
    # Each <stem> draws through the shared harness: uml_gpu_<stem>_tests,
    # built from <stem>_tests.cpp, registers as gpu-<stem with dashes>.
    foreach(stem
            vulkan_device_address_commands kosmickrisp_geometry_invocations
            kosmickrisp_partial_position kosmickrisp_pipeline_statistics
            kosmickrisp_point_clipping kosmickrisp_tessellation_feedback
            kosmickrisp_command_storage kosmickrisp_raster_polygon
            kosmickrisp_raster_line kosmickrisp_raster_state
            kosmickrisp_geometry_side_effects)
        string(REPLACE "_" "-" test_name "gpu-${stem}")
        uml_add_gpu_unit_test(${test_name} uml_gpu_${stem}_tests
            ${stem}_tests.cpp
            LIBRARIES uml_gpu_vulkan_draw_test_support
            VULKAN)
    endforeach()
    foreach(stem geometry_invocations raster_line geometry_side_effects)
        target_link_libraries(uml_gpu_kosmickrisp_${stem}_tests PRIVATE
            Threads::Threads)
    endforeach()
endif()

if(TARGET uml_gpu_service)
    uml_add_gpu_unit_test(gpu-host-gl-provider-lifetime
        uml_gpu_host_gl_provider_tests host_gl_provider_lifetime_tests.cpp
        LIBRARIES uml_gpu_renderer UML::ThirdPartyVirglrenderer
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # The host GL provider has to be able to alias memory the caller already
    # owns as a sampled texture; the guest scanout import depends on it.
    uml_add_gpu_unit_test(gpu-host-pointer-image
        uml_gpu_host_pointer_image_tests host_gl_host_pointer_image_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # A transfer a context refuses must not leave that context refusing every
    # later transfer: Android's gralloc mapper only ever transfers.
    uml_add_gpu_unit_test(gpu-transfer-context-error
        uml_gpu_transfer_context_error_tests
        host_gl_transfer_context_error_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # A blob texture has no guest pages, so a guest reads and writes it through
    # a staging buffer: the copy transfer must check the staging buffer for
    # guest storage in both directions, not the texture.
    uml_add_gpu_unit_test(gpu-blob-copy-transfer
        uml_gpu_blob_copy_transfer_tests
        host_gl_blob_copy_transfer_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # A new texture reads as zeros, not as the pixels of the resource that
    # released its memory: the guest allocated zeroed pages, and the leftover
    # pixels may belong to another process.
    uml_add_gpu_unit_test(gpu-texture-zero-fill
        uml_gpu_texture_zero_fill_tests
        host_gl_texture_zero_fill_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # A layout the guest names has to fit the host storage behind it.
    uml_add_gpu_unit_test(gpu-import-bounds
        uml_gpu_import_bounds_tests host_gl_import_bounds_tests.mm
        LIBRARIES ${_uml_host_gl_libraries}
            "-framework Foundation" "-framework Metal"
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # The native display samples the Metal texture that stores a Virgl
    # scanout's GL texture, so the renderer must name the right one: its size,
    # format, orientation, device, and fenced contents.
    uml_add_gpu_unit_test(gpu-resource-metal-texture
        uml_gpu_resource_metal_texture_tests
        host_gl_resource_metal_texture_tests.mm
        LIBRARIES ${_uml_host_gl_libraries}
            "-framework Foundation" "-framework Metal"
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # One thread drives every host GL context, so a read in one context of a
    # texture another context left unsubmitted has to finish without a second
    # thread. Blocking there parks the GPU thread and wedges the whole guest.
    uml_add_gpu_unit_test(gpu-cross-context-readback
        uml_gpu_cross_context_readback_tests
        host_gl_cross_context_readback_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # Virglrenderer advertises depth formats the guest turns into default
    # framebuffers. A host that substitutes different storage for a sized depth
    # format leaves an OpenGL ES guest unable to name its own depth buffer.
    uml_add_gpu_unit_test(gpu-depth-format-advertisement
        uml_gpu_depth_format_advertisement_tests
        host_gl_depth_format_advertisement_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # GL_VERTICES_SUBMITTED_ARB counts submitted vertices, not assembled ones.
    # Zink does not advertise LINE_LOOP, so Mesa decomposes every line loop
    # into independent lines before the driver sees it, doubling the count
    # unless the driver subtracts the conversion back out. A clipping query
    # active beside it, which Zink begins as a second Vulkan statistics query,
    # must still count the loop's clipping input primitives.
    uml_add_gpu_unit_test(gpu-line-loop-statistics
        uml_gpu_line_loop_statistics_tests
        host_gl_line_loop_statistics_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # Zink keeps the first result it reads when a poll finds a statistics
    # query available, so the host driver must not publish availability
    # before the counts.
    uml_add_gpu_unit_test(gpu-statistics-availability
        uml_gpu_statistics_availability_tests
        host_gl_statistics_availability_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # A host without a usable GL stack or renderer skips these with status 77.
    set_tests_properties(
        gpu-transfer-context-error gpu-blob-copy-transfer
        gpu-texture-zero-fill gpu-import-bounds
        gpu-resource-metal-texture gpu-cross-context-readback
        gpu-depth-format-advertisement gpu-line-loop-statistics
        gpu-statistics-availability
        PROPERTIES SKIP_RETURN_CODE 77)

    # The renderer and the same-process render server share one fence table.
    # Exercise that ownership without a device: it is a pure lifetime
    # contract, and losing it faults the GPU thread.
    uml_add_gpu_unit_test(gpu-virgl-fence-table-ownership
        uml_gpu_virgl_fence_table_tests virgl_fence_table_ownership_tests.cpp
        LIBRARIES UML::ThirdPartyVirglrenderer
        UNIT_DIRECTORY)

    uml_add_gpu_unit_test(gpu-transport-wire-ownership
        uml_gpu_transport_wire_tests
        vhost_user_message_tests.cpp
        LIBRARIES
            uml_gpu_transport_wire_stop_wait_test_variant
            uml_test_allocation_failure
        INCLUDES ${_uml_gpu_device_directory} ${CMAKE_CURRENT_SOURCE_DIR}
        UNIT_DIRECTORY)

    # Its own executable: the descriptor ceiling is process state, and filling
    # the file table would decide the outcome of anything sharing the process.
    uml_add_gpu_unit_test(gpu-transport-descriptor-ceiling
        uml_gpu_transport_ceiling_tests
        vhost_user_descriptor_ceiling_tests.cpp
        LIBRARIES uml_vhost_user_wire
        INCLUDES ${_uml_gpu_device_directory} ${CMAKE_CURRENT_SOURCE_DIR}
        UNIT_DIRECTORY)

    uml_add_gpu_unit_test(gpu-service-fd-ownership
        uml_gpu_service_fd_tests
        gpu_lifetime_tests.cpp
        in_process_service_testing.cpp
        in_process_service_lifetime_tests.cpp
        vhost_user_protocol_tests.cpp
        vhost_user_server_lifetime_tests.cpp
        LIBRARIES uml_gpu_service_test_variant
        INCLUDES ${_uml_gpu_device_directory} ${CMAKE_CURRENT_SOURCE_DIR}
        UNIT_DIRECTORY)

    uml_add_gpu_unit_test(gpu-service-stop-wake
        uml_gpu_service_stop_wake_tests
        in_process_service_stop_wake_tests.cpp
        in_process_service_testing.cpp
        LIBRARIES uml_gpu_service_slow_stop_test_variant
        INCLUDES
            ${_uml_gpu_device_directory}
            ${PROJECT_SOURCE_DIR}/darwin/device/vhost_user
            ${CMAKE_CURRENT_SOURCE_DIR}
        UNIT_DIRECTORY)

    uml_add_gpu_unit_test(gpu-device-command-atomicity
        uml_gpu_device_atomicity_tests
        virtio_gpu_device_lifetime_tests.cpp
        LIBRARIES
            uml_gpu_protocol_device_test_variant
            uml_test_allocation_failure
        INCLUDES ${_uml_gpu_device_directory} ${CMAKE_CURRENT_SOURCE_DIR}
        UNIT_DIRECTORY)
endif()

if(TARGET uml_gpu_display)
    # The resize path's objects come from the shipped display library.
    uml_add_gpu_unit_test(gpu-display-resize
        uml_gpu_display_resize_tests display_resize_tests.cpp
        LIBRARIES uml_gpu_display
        UNIT_DIRECTORY)

    uml_add_gpu_unit_test(gpu-vnc-lifetime
        uml_gpu_vnc_lifetime_tests
        vnc_display_lifetime_tests.cpp
        vnc_display_metal_testing.mm
        vnc_display_testing.cpp
        LIBRARIES uml_gpu_display_test_variant
            "-framework Foundation" "-framework Metal"
        UNIT_DIRECTORY)

    # This one links the shipped display library rather than a test variant,
    # so it also answers whether the product was built with the native Metal
    # readback that a Venus scanout has no alternative to.
    if(UML_TARGET_MACOS)
        uml_add_gpu_unit_test(gpu-vnc-product-readback
            uml_gpu_vnc_product_tests vnc_display_product_tests.mm
            LIBRARIES uml_gpu_display
                "-framework Foundation" "-framework Metal"
            UNIT_DIRECTORY)

        # The shipped display must return when the guest finishes while a
        # menu is open. It needs a window server session and skips without
        # one.
        uml_add_gpu_unit_test(gpu-native-display-menu-teardown
            uml_gpu_native_display_menu_teardown_tests
            native_display_menu_teardown_tests.mm
            LIBRARIES uml_gpu_display
                "-framework AppKit" "-framework CoreGraphics"
            INCLUDES ${_uml_gpu_device_directory}
            UNIT_DIRECTORY TIMEOUT 90)
        set_tests_properties(gpu-native-display-menu-teardown PROPERTIES
            SKIP_RETURN_CODE 77)

        # The native presenter must scale a scanout whose size or rectangle
        # differs from its framebuffer. It draws into an offscreen view and
        # skips on a host without a Metal device or offscreen drawables.
        uml_add_gpu_unit_test(gpu-native-metal-presentation
            uml_gpu_metal_presentation_tests metal_presentation_tests.mm
            LIBRARIES uml_gpu_display
                "-framework AppKit" "-framework CoreVideo"
                "-framework Metal" "-framework MetalKit"
                "-framework QuartzCore"
            INCLUDES ${_uml_gpu_device_directory}
            UNIT_DIRECTORY TIMEOUT 90)
        set_tests_properties(gpu-native-metal-presentation PROPERTIES
            SKIP_RETURN_CODE 77)
    endif()
endif()

unset(_uml_unit_binary_directory)
unset(_uml_gpu_device_directory)
unset(_uml_host_gl_libraries)
