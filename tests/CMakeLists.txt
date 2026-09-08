# SPDX-License-Identifier: GPL-2.0-or-later

# The standalone GPU server and VNC client are host test fixtures. Build them
# when either their tooling smoke or the external integration workflows own a
# use, without making either suite depend on the other suite being enabled.
set(_uml_need_vnc_fixtures OFF)
if(UML_BUILD_TOOLING_TESTS OR
   (UML_BUILD_INTEGRATION_TESTS AND UML_ENABLE_EXTERNAL_NETWORK_TESTS))
    set(_uml_need_vnc_fixtures ON)
endif()
if(_uml_need_vnc_fixtures AND TARGET uml_gpu_service)
    add_executable(uml-virtio-gpu
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/virtio_gpu_fixture.cpp)
    target_link_libraries(uml-virtio-gpu PRIVATE UML::GpuDevice)
    target_include_directories(uml-virtio-gpu PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    target_compile_features(uml-virtio-gpu PRIVATE cxx_std_20)
    target_compile_options(uml-virtio-gpu PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    set_target_properties(uml-virtio-gpu PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

    add_executable(uml-vnc-smoke-client
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/vnc_smoke_client.cpp)
    target_link_libraries(uml-vnc-smoke-client PRIVATE
        UML::ThirdPartyVncClient
        "-framework CoreFoundation")
    target_compile_options(uml-vnc-smoke-client PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    set_target_properties(uml-vnc-smoke-client PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()

# Manual benchmark payloads have explicit source ownership but remain outside
# all correctness-suite aggregates and registrations.
add_subdirectory(perf EXCLUDE_FROM_ALL)

# Both suites execute this native wrapper: unit registrations use it to write
# instruction evidence, while a tooling-only graph uses it as a fixture for
# the raw-evidence compression tests.  Own it at the shared tests boundary so
# either suite can be configured independently.
if(UML_BUILD_UNIT_TESTS OR UML_BUILD_TOOLING_TESTS)
    add_executable(uml_instruction_evidence_test_runner
        unit/arm64/instruction_evidence_test_runner.cpp)
    target_compile_features(uml_instruction_evidence_test_runner PRIVATE
        cxx_std_20)
    target_compile_options(uml_instruction_evidence_test_runner PRIVATE
        -Wall -Wextra -Werror)
endif()

if(UML_BUILD_UNIT_TESTS)
    add_library(uml_host_capabilities_test_support OBJECT
        unit/host_capabilities_test_support.cpp)
    target_link_libraries(uml_host_capabilities_test_support PRIVATE
        UML::CpuContract
        UML::PlatformContract)
    target_include_directories(uml_host_capabilities_test_support PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_features(uml_host_capabilities_test_support PRIVATE
        cxx_std_20)
    target_compile_options(uml_host_capabilities_test_support PRIVATE
        -Wall -Wextra -Werror)
    # Unit binaries are independent consumers of the same scoped build
    # configuration as production targets; no directory-global definitions
    # are needed.
    link_libraries(
        uml_host_capabilities_test_support
        UML::GuestPageConfiguration
        UML::JitProfileConfiguration)
    add_subdirectory(unit)
endif()

if(UML_BUILD_TOOLING_TESTS)
    add_subdirectory(tooling)
    add_custom_target(tooling-test-binaries)
    add_dependencies(tooling-test-binaries
        uml_arm64_execution_helpers
        uml_arm64_gadgets
        uml_arm64_jit
        uml_arm64_jit_executor)
    if(TARGET uml_arm64_engine_test_support_tests)
        add_dependencies(tooling-test-binaries
            uml_arm64_engine_test_support_tests)
    endif()
endif()

if(UML_BUILD_INTEGRATION_TESTS)
    # CTest commands reference these host fixtures directly. Keep their build
    # ownership explicit so the integration preset produces every executable
    # required by the registered suite, not only the emulator app.
    add_custom_target(integration-test-binaries)
    add_subdirectory(integration)
    if(UML_BUILD_WINE_ARM64EC)
        set(_integration_targets uml-darwin-wine-arm64ec)
    else()
        set(_integration_targets uml-darwin)
    endif()
    if(UML_ENABLE_EXTERNAL_NETWORK_TESTS)
        list(APPEND _integration_targets
            uml-virtio-gpu uml-vnc-smoke-client)
    endif()
    foreach(_integration_target IN LISTS _integration_targets)
        if(NOT TARGET ${_integration_target})
            message(FATAL_ERROR
                "Integration target is not available: ${_integration_target}")
        endif()
        add_dependencies(integration-test-binaries ${_integration_target})
    endforeach()
endif()

# Suite aggregates are derived from the actual target-backed CTest commands,
# including commands whose fixtures are owned by another directory. A build
# preset therefore cannot finish successfully while leaving a registered test
# executable absent from a fresh graph.
if(UML_BUILD_UNIT_TESTS)
    uml_finalize_suite_binary_target(unit unit-test-binaries)
endif()
if(UML_BUILD_TOOLING_TESTS)
    uml_finalize_suite_binary_target(tooling tooling-test-binaries)
endif()
if(UML_BUILD_INTEGRATION_TESTS)
    uml_finalize_suite_binary_target(integration integration-test-binaries)
endif()

# Every CTest registration goes through an explicit suite helper.  Validate
# that it has a retained manifest entry before generation completes; this
# rejects accidental test additions, omissions, and suite drift immediately.
uml_validate_registered_test_manifest()

set(_uml_enabled_test_suites)
if(UML_BUILD_TOOLING_TESTS)
    list(APPEND _uml_enabled_test_suites tooling)
endif()
if(UML_BUILD_UNIT_TESTS)
    list(APPEND _uml_enabled_test_suites unit)
endif()
if(UML_BUILD_INTEGRATION_TESTS)
    list(APPEND _uml_enabled_test_suites integration)
endif()
string(JOIN "," _uml_enabled_test_suites_csv ${_uml_enabled_test_suites})

# The manifest keeps configuration-specific coverage visible without making a
# 4 KiB-only test appear missing from the 16 KiB registry. The audit receives
# the active conditional registrations explicitly, so it still rejects an
# accidental omission from the configuration where the test applies.
set(_uml_required_conditional_tests)
if(UML_BUILD_UNIT_TESTS AND UML_BUILD_X86_64_GUEST)
    list(APPEND _uml_required_conditional_tests
        native-4k-shadow-mmu
        emulator-native-4k-admission
        x86-64-gadget-bootstrap
        x86-64-instruction-corpus)
endif()
if(UML_BUILD_INTEGRATION_TESTS AND UML_BUILD_WINE_ARM64EC)
    if(UML_TEST_STEAM_ARM64EC)
        list(APPEND _uml_required_conditional_tests guest-steam-arm64ec-smoke)
    else()
        list(APPEND _uml_required_conditional_tests guest-wine-arm64ec-smoke)
    endif()
endif()
if(UML_BUILD_UNIT_TESTS AND UML_GUEST_16K_PAGES)
    list(APPEND _uml_required_conditional_tests
        emulator-native-16k-ownership)
endif()
# The cross-executor translation store is an experimental arm, off by default,
# and its gate only exists where it is enabled. See G161.
if(UML_BUILD_UNIT_TESTS AND UML_GADGET_SHARED_BUDGET_MIB GREATER 0)
    list(APPEND _uml_required_conditional_tests
        arm64-gadget-shared-translation)
endif()
if(UML_BUILD_UNIT_TESTS AND UML_BUILD_TOOLING_TESTS)
    list(APPEND _uml_required_conditional_tests
        arm64-instruction-evidence-reset
        arm64-instruction-evidence)
    if(UML_INSTRUCTION_INPUT_MATERIALIZATION)
        list(APPEND _uml_required_conditional_tests
            arm64-input-materialization-join)
    endif()
endif()
if(UML_BUILD_UNIT_TESTS)
    if(DARWIN_UML_JIT_GPR_LINK_PROFILE)
        list(APPEND _uml_required_conditional_tests
            arm64-jit-gpr-link-profile-codegen
            arm64-jit-gpr-link-profile-runtime)
    endif()
    if(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
        list(APPEND _uml_required_conditional_tests
            arm64-jit-gpr-pressure-profile)
    endif()
endif()
if(TARGET uml_jit_profile_final_report_fixture)
    list(APPEND _uml_required_conditional_tests
        jit-profile-final-report-contract)
endif()
if(TARGET uml_slirp_network_tests)
    list(APPEND _uml_required_conditional_tests slirp-network-arp)
endif()
if(TARGET uml_slirp_forward_tests)
    list(APPEND _uml_required_conditional_tests
        slirp-network-forward-specification)
endif()
if(UML_BUILD_UNIT_TESTS AND TARGET uml_gpu_service)
    list(APPEND _uml_required_conditional_tests
        gpu-transport-wire-ownership)
endif()
if(TARGET uml_gpu_service_fd_tests)
    list(APPEND _uml_required_conditional_tests gpu-service-fd-ownership)
endif()
if(TARGET uml_gpu_service_stop_wake_tests)
    list(APPEND _uml_required_conditional_tests gpu-service-stop-wake)
endif()
if(TARGET uml_gpu_host_gl_provider_tests)
    list(APPEND _uml_required_conditional_tests gpu-host-gl-provider-lifetime)
endif()
if(TARGET uml_gpu_kosmickrisp_external_image_alias_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-external-image-alias)
endif()
if(TARGET uml_gpu_kosmickrisp_timestamp_availability_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-timestamp-availability)
endif()
if(TARGET uml_gpu_kosmickrisp_device_address_commands_capability_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-device-address-commands-capabilities)
endif()
if(TARGET uml_gpu_vulkan_device_address_commands_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-vulkan-device-address-commands)
endif()
if(TARGET uml_gpu_kosmickrisp_dynamic_logic_op_capability_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-dynamic-logic-op-capabilities)
endif()
if(TARGET uml_gpu_kosmickrisp_line_multiview_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-line-multiview)
endif()
if(TARGET uml_gpu_kosmickrisp_geometry_side_effects_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-geometry-side-effects)
endif()
if(TARGET uml_gpu_kosmickrisp_geometry_invocations_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-geometry-invocations)
endif()
if(TARGET uml_gpu_kosmickrisp_pipeline_statistics_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-pipeline-statistics)
endif()
if(TARGET uml_gpu_kosmickrisp_point_clipping_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-point-clipping)
endif()
if(TARGET uml_gpu_kosmickrisp_tessellation_feedback_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-tessellation-feedback)
endif()
if(TARGET uml_gpu_kosmickrisp_raster_polygon_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-raster-polygon)
endif()
if(TARGET uml_gpu_kosmickrisp_raster_line_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-raster-line)
endif()
if(TARGET uml_gpu_kosmickrisp_raster_state_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-raster-state)
endif()
if(TARGET uml_gpu_kosmickrisp_unused_attachments_capability_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-unused-attachments-capabilities)
endif()
if(TARGET uml_gpu_kosmickrisp_graphics_pipeline_library_capability_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-graphics-pipeline-library-capabilities)
endif()
if(TARGET uml_gpu_kosmickrisp_legacy_vertex_attribute_capability_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-legacy-vertex-attribute-capabilities)
endif()
if(TARGET uml_gpu_kosmickrisp_legacy_vertex_input_draw_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-legacy-vertex-input-draws)
endif()
if(TARGET uml_gpu_kosmickrisp_non_seamless_cube_capability_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-non-seamless-cube-capabilities)
endif()
if(TARGET uml_gpu_kosmickrisp_rgb32_texel_capability_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-rgb32-texel-capabilities)
endif()
if(TARGET uml_gpu_kosmickrisp_incremental_present_capability_tests)
    list(APPEND _uml_required_conditional_tests
        gpu-kosmickrisp-incremental-present-capabilities)
endif()
if(TARGET uml_gpu_device_atomicity_tests)
    list(APPEND _uml_required_conditional_tests gpu-device-command-atomicity)
endif()
if(TARGET uml_gpu_display_resize_tests)
    list(APPEND _uml_required_conditional_tests gpu-display-resize)
endif()
if(TARGET uml_gpu_vnc_lifetime_tests)
    list(APPEND _uml_required_conditional_tests gpu-vnc-lifetime)
endif()
if(TARGET uml_virtio_input_tests)
    list(APPEND _uml_required_conditional_tests virtio-input-protocol)
endif()
if(TARGET uml_virtio_sound_tests)
    list(APPEND _uml_required_conditional_tests virtio-sound-protocol)
endif()
if(TARGET uml_virtio_battery_tests)
    list(APPEND _uml_required_conditional_tests virtio-battery-protocol)
endif()
if(TARGET uml_virtio_video_tests)
    list(APPEND _uml_required_conditional_tests virtio-video-protocol)
endif()
if(UML_BUILD_INTEGRATION_TESTS AND UML_V4L2_PROBE_BINARY)
    list(APPEND _uml_required_conditional_tests guest-v4l2-decode)
endif()
if(TARGET uml_virtio_console_tests)
    list(APPEND _uml_required_conditional_tests virtio-console-protocol)
endif()
if(TARGET uml_embedded_kernel_serialization_tests)
    list(APPEND _uml_required_conditional_tests
        embedded-kernel-handle-serialization)
endif()
if(UML_BUILD_TOOLING_TESTS AND TARGET uml-vnc-smoke-client)
    list(APPEND _uml_required_conditional_tests virtio-gpu-vnc-smoke)
endif()
if(UML_BUILD_INTEGRATION_TESTS AND UML_ENABLE_EXTERNAL_NETWORK_TESTS)
    list(APPEND _uml_required_conditional_tests
        guest-external-network
        guest-kde-vnc-webgl-aquarium
        guest-webgl-rendered-workflows)
endif()
string(JOIN "," _uml_required_conditional_tests_csv
    ${_uml_required_conditional_tests})
set_property(GLOBAL PROPERTY UML_AUDIT_EXPECTED_TEST_SUITES
    "${_uml_enabled_test_suites_csv}")
set_property(GLOBAL PROPERTY UML_AUDIT_REQUIRED_CONDITIONAL_TESTS
    "${_uml_required_conditional_tests_csv}")
