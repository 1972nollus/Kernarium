# SPDX-License-Identifier: GPL-2.0-or-later

cmake_path(GET CMAKE_CURRENT_BINARY_DIR PARENT_PATH
    _uml_unit_binary_directory)

if(TARGET uml_gpu_service)
    add_executable(uml_gpu_host_gl_provider_tests
        host_gl_provider_lifetime_tests.cpp)
    target_link_libraries(uml_gpu_host_gl_provider_tests PRIVATE
        uml_gpu_renderer
        UML::ThirdPartyVirglrenderer)
    target_include_directories(uml_gpu_host_gl_provider_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    target_compile_features(uml_gpu_host_gl_provider_tests PRIVATE cxx_std_20)
    target_compile_options(uml_gpu_host_gl_provider_tests PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-host-gl-provider-lifetime
        COMMAND uml_gpu_host_gl_provider_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu)

    add_executable(uml_gpu_transport_wire_tests
        gpu_allocation_failure.cpp
        vhost_user_message_test_main.cpp
        vhost_user_message_tests.cpp)
    target_link_libraries(uml_gpu_transport_wire_tests PRIVATE
        uml_gpu_transport_wire)
    target_include_directories(uml_gpu_transport_wire_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(uml_gpu_transport_wire_tests PRIVATE cxx_std_20)
    target_compile_options(uml_gpu_transport_wire_tests PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-transport-wire-ownership
        COMMAND uml_gpu_transport_wire_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu)

    add_library(uml_gpu_service_test_variant STATIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu/in_process_service.cpp
        in_process_service_testing.cpp)
    target_link_libraries(uml_gpu_service_test_variant PRIVATE
        uml_gpu_transport)
    target_include_directories(uml_gpu_service_test_variant PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(uml_gpu_service_test_variant PRIVATE
        UML_GPU_STARTUP_TESTING=1)
    target_compile_features(uml_gpu_service_test_variant PUBLIC cxx_std_20)
    target_compile_options(uml_gpu_service_test_variant PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)

    add_executable(uml_gpu_service_fd_tests
        gpu_lifetime_tests.cpp
        in_process_service_lifetime_tests.cpp
        vhost_user_protocol_tests.cpp
        vhost_user_server_lifetime_tests.cpp)
    target_link_libraries(uml_gpu_service_fd_tests PRIVATE
        uml_gpu_service_test_variant)
    target_include_directories(uml_gpu_service_fd_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(uml_gpu_service_fd_tests PRIVATE cxx_std_20)
    target_compile_options(uml_gpu_service_fd_tests PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-service-fd-ownership
        COMMAND uml_gpu_service_fd_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu)

    # A deliberately slow test transport makes the owned stop wake
    # observable: the child watchdog fails if service shutdown falls back to
    # periodic polling.
    add_library(uml_gpu_transport_wire_slow_stop_test_variant STATIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu/vhost_user_message.cpp)
    target_include_directories(uml_gpu_transport_wire_slow_stop_test_variant
        PUBLIC ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    target_compile_definitions(uml_gpu_transport_wire_slow_stop_test_variant
        PRIVATE
            UML_GPU_STOP_POLL_TIMEOUT_MILLISECONDS=5000
            UML_GPU_STOP_WAIT_TESTING=1)
    target_compile_features(uml_gpu_transport_wire_slow_stop_test_variant
        PUBLIC cxx_std_20)
    target_compile_options(uml_gpu_transport_wire_slow_stop_test_variant PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)

    add_library(uml_gpu_transport_slow_stop_test_variant STATIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu/vhost_user_server.cpp)
    target_include_directories(uml_gpu_transport_slow_stop_test_variant
        PUBLIC ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    target_compile_definitions(uml_gpu_transport_slow_stop_test_variant
        PRIVATE UML_GPU_STOP_POLL_TIMEOUT_MILLISECONDS=5000)
    target_compile_features(uml_gpu_transport_slow_stop_test_variant
        PUBLIC cxx_std_20)
    target_compile_options(uml_gpu_transport_slow_stop_test_variant PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    target_link_libraries(uml_gpu_transport_slow_stop_test_variant PUBLIC
        UML::DeviceSocketPaths
        uml_gpu_transport_wire_slow_stop_test_variant
        uml_gpu_protocol_device
        UML::VncInput)

    add_library(uml_gpu_service_slow_stop_test_variant STATIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu/in_process_service.cpp)
    target_include_directories(uml_gpu_service_slow_stop_test_variant PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    target_compile_definitions(uml_gpu_service_slow_stop_test_variant PRIVATE
        UML_GPU_STARTUP_TESTING=0)
    target_compile_features(uml_gpu_service_slow_stop_test_variant PUBLIC
        cxx_std_20)
    target_compile_options(uml_gpu_service_slow_stop_test_variant PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    target_link_libraries(uml_gpu_service_slow_stop_test_variant PRIVATE
        uml_gpu_transport_slow_stop_test_variant)

    add_executable(uml_gpu_service_stop_wake_tests
        in_process_service_stop_wake_tests.cpp)
    target_include_directories(uml_gpu_service_stop_wake_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(uml_gpu_service_stop_wake_tests PRIVATE cxx_std_20)
    target_compile_options(uml_gpu_service_stop_wake_tests PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    target_link_libraries(uml_gpu_service_stop_wake_tests PRIVATE
        uml_gpu_service_slow_stop_test_variant)
    uml_add_unit_test(NAME gpu-service-stop-wake
        COMMAND uml_gpu_service_stop_wake_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu)

    add_library(uml_gpu_protocol_device_test_variant STATIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu/virtio_gpu_device.cpp)
    target_include_directories(uml_gpu_protocol_device_test_variant PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(uml_gpu_protocol_device_test_variant PUBLIC
        UML_GPU_DEVICE_TESTING=1)
    target_compile_features(uml_gpu_protocol_device_test_variant PUBLIC
        cxx_std_20)
    target_compile_options(uml_gpu_protocol_device_test_variant PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)

    add_executable(uml_gpu_device_atomicity_tests
        gpu_allocation_failure.cpp
        virtio_gpu_device_test_main.cpp
        virtio_gpu_device_lifetime_tests.cpp)
    target_link_libraries(uml_gpu_device_atomicity_tests PRIVATE
        uml_gpu_protocol_device_test_variant)
    target_include_directories(uml_gpu_device_atomicity_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(uml_gpu_device_atomicity_tests PRIVATE cxx_std_20)
    target_compile_options(uml_gpu_device_atomicity_tests PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-device-command-atomicity
        COMMAND uml_gpu_device_atomicity_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu)
endif()

if(TARGET uml_gpu_display)
    add_library(uml_gpu_display_test_variant STATIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu/scanout_framebuffer.cpp
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu/vnc_display.cpp
        vnc_display_testing.cpp)
    target_link_libraries(uml_gpu_display_test_variant PUBLIC
        UML::VncInput)
    target_link_libraries(uml_gpu_display_test_variant PRIVATE
        UML::ThirdPartyVncServer
        "-framework Accelerate")
    target_include_directories(uml_gpu_display_test_variant PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${PROJECT_SOURCE_DIR}/darwin/device/input/vnc
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(uml_gpu_display_test_variant PRIVATE
        UML_GPU_VNC_STARTUP_TESTING=1)
    target_compile_features(uml_gpu_display_test_variant PUBLIC cxx_std_20)
    target_compile_options(uml_gpu_display_test_variant PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    add_dependencies(uml_gpu_display_test_variant
        uml_dependency_prefix_verify)

    add_executable(uml_gpu_vnc_lifetime_tests
        vnc_display_lifetime_tests.cpp)
    target_link_libraries(uml_gpu_vnc_lifetime_tests PRIVATE
        uml_gpu_display_test_variant)
    target_compile_features(uml_gpu_vnc_lifetime_tests PRIVATE cxx_std_20)
    target_compile_options(uml_gpu_vnc_lifetime_tests PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-vnc-lifetime
        COMMAND uml_gpu_vnc_lifetime_tests
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        LABELS gpu)
endif()

unset(_uml_unit_binary_directory)
