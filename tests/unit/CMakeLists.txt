# SPDX-License-Identifier: GPL-2.0-or-later

if(UML_BUILD_TOOLING_TESTS)
    # The CMake-only reset belongs to the unit suite and is registered only
    # when the tooling validators consume the evidence produced by unit tests.
    # A strict unit selection in that combined graph therefore clears stale
    # evidence before running any producer.
    uml_add_unit_test(
        NO_EVIDENCE_DEPENDENCY
        NAME arm64-instruction-evidence-reset
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_ROOT=${CMAKE_BINARY_DIR}
            -P ${PROJECT_SOURCE_DIR}/tests/unit/arm64/reset_instruction_evidence.cmake
        LABELS cpu;audit)
endif()

add_library(uml_test_allocation_failure STATIC allocation_failure.cpp)
target_include_directories(uml_test_allocation_failure PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_test_allocation_failure PUBLIC cxx_std_20)
target_compile_options(uml_test_allocation_failure PRIVATE
    -Wall -Wextra -Werror)

# CPU conformance and engine/runtime tests have their own directory scope.
# This keeps the suite entry point small while preserving one explicit source
# manifest per test domain.
add_subdirectory(arm64)
add_subdirectory(memory)
add_subdirectory(runtime)

add_executable(uml_emulator_c_api_boundary_tests
    emulator_c_api_boundary_tests.cpp)
target_link_libraries(uml_emulator_c_api_boundary_tests PRIVATE UML::Emulator)
target_include_directories(uml_emulator_c_api_boundary_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include)
target_compile_features(uml_emulator_c_api_boundary_tests PRIVATE cxx_std_20)
target_compile_options(uml_emulator_c_api_boundary_tests PRIVATE
    -Wall -Wextra -Werror)
add_dependencies(unit-test-binaries uml_emulator_c_api_boundary_tests)
uml_add_unit_test(
    NAME emulator-c-api-boundary
    COMMAND uml_emulator_c_api_boundary_tests)

add_executable(uml_vnc_input_tests gpu/vnc_input_tests.cpp)
target_link_libraries(uml_vnc_input_tests PRIVATE UML::VncInput)
target_include_directories(uml_vnc_input_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/device/input/vnc)
target_compile_features(uml_vnc_input_tests PRIVATE cxx_std_20)
target_compile_options(uml_vnc_input_tests PRIVATE
    -Wall -Wextra -Werror)
add_dependencies(unit-test-binaries uml_vnc_input_tests)
uml_add_unit_test(NAME vnc-input-boundary COMMAND uml_vnc_input_tests
    LABELS input)

if(TARGET uml_gpu_service)
    add_executable(uml_gpu_transport_wire_tests
        gpu/gpu_allocation_failure.cpp
        gpu/vhost_user_message_test_main.cpp
        gpu/vhost_user_message_tests.cpp)
    target_link_libraries(uml_gpu_transport_wire_tests PRIVATE
        uml_gpu_transport_wire)
    target_include_directories(uml_gpu_transport_wire_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR}/gpu)
    target_compile_features(uml_gpu_transport_wire_tests PRIVATE cxx_std_20)
    target_compile_options(uml_gpu_transport_wire_tests PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-transport-wire-ownership
        COMMAND uml_gpu_transport_wire_tests
        LABELS gpu)

    add_library(uml_gpu_service_test_variant STATIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu/in_process_service.cpp
        gpu/in_process_service_testing.cpp)
    target_link_libraries(uml_gpu_service_test_variant PRIVATE
        uml_gpu_transport)
    target_include_directories(uml_gpu_service_test_variant PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR}/gpu)
    target_compile_definitions(uml_gpu_service_test_variant PRIVATE
        UML_GPU_STARTUP_TESTING=1)
    target_compile_features(uml_gpu_service_test_variant PUBLIC cxx_std_20)
    target_compile_options(uml_gpu_service_test_variant PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)

    add_executable(uml_gpu_service_fd_tests
        gpu/gpu_lifetime_tests.cpp
        gpu/in_process_service_lifetime_tests.cpp
        gpu/vhost_user_protocol_tests.cpp
        gpu/vhost_user_server_lifetime_tests.cpp)
    target_link_libraries(uml_gpu_service_fd_tests PRIVATE
        uml_gpu_service_test_variant)
    target_include_directories(uml_gpu_service_fd_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR}/gpu)
    target_compile_features(uml_gpu_service_fd_tests PRIVATE cxx_std_20)
    target_compile_options(uml_gpu_service_fd_tests PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-service-fd-ownership
        COMMAND uml_gpu_service_fd_tests
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
        gpu/in_process_service_stop_wake_tests.cpp)
    target_include_directories(uml_gpu_service_stop_wake_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR}/gpu)
    target_compile_features(uml_gpu_service_stop_wake_tests PRIVATE cxx_std_20)
    target_compile_options(uml_gpu_service_stop_wake_tests PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    target_link_libraries(uml_gpu_service_stop_wake_tests PRIVATE
        uml_gpu_service_slow_stop_test_variant)
    uml_add_unit_test(NAME gpu-service-stop-wake
        COMMAND uml_gpu_service_stop_wake_tests
        LABELS gpu)

    add_library(uml_gpu_protocol_device_test_variant STATIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu/virtio_gpu_device.cpp)
    target_include_directories(uml_gpu_protocol_device_test_variant PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR}/gpu)
    target_compile_definitions(uml_gpu_protocol_device_test_variant PUBLIC
        UML_GPU_DEVICE_TESTING=1)
    target_compile_features(uml_gpu_protocol_device_test_variant PUBLIC
        cxx_std_20)
    target_compile_options(uml_gpu_protocol_device_test_variant PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)

    add_executable(uml_gpu_device_atomicity_tests
        gpu/gpu_allocation_failure.cpp
        gpu/virtio_gpu_device_test_main.cpp
        gpu/virtio_gpu_device_lifetime_tests.cpp)
    target_link_libraries(uml_gpu_device_atomicity_tests PRIVATE
        uml_gpu_protocol_device_test_variant)
    target_include_directories(uml_gpu_device_atomicity_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${CMAKE_CURRENT_SOURCE_DIR}/gpu)
    target_compile_features(uml_gpu_device_atomicity_tests PRIVATE cxx_std_20)
    target_compile_options(uml_gpu_device_atomicity_tests PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-device-command-atomicity
        COMMAND uml_gpu_device_atomicity_tests
        LABELS gpu)
endif()

if(TARGET uml_gpu_display)
    add_library(uml_gpu_display_test_variant STATIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu/vnc_display.cpp
        gpu/vnc_display_testing.cpp)
    target_link_libraries(uml_gpu_display_test_variant PUBLIC
        UML::VncInput)
    target_link_libraries(uml_gpu_display_test_variant PRIVATE
        UML::ThirdPartyVncServer
        "-framework Accelerate")
    target_include_directories(uml_gpu_display_test_variant PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${PROJECT_SOURCE_DIR}/darwin/device/input/vnc
        ${CMAKE_CURRENT_SOURCE_DIR}/gpu)
    target_compile_definitions(uml_gpu_display_test_variant PRIVATE
        UML_GPU_VNC_STARTUP_TESTING=1)
    target_compile_features(uml_gpu_display_test_variant PUBLIC cxx_std_20)
    target_compile_options(uml_gpu_display_test_variant PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    add_dependencies(uml_gpu_display_test_variant
        uml_dependency_prefix_verify)

    add_executable(uml_gpu_vnc_lifetime_tests
        gpu/vnc_display_lifetime_tests.cpp)
    target_link_libraries(uml_gpu_vnc_lifetime_tests PRIVATE
        uml_gpu_display_test_variant)
    target_compile_features(uml_gpu_vnc_lifetime_tests PRIVATE cxx_std_20)
    target_compile_options(uml_gpu_vnc_lifetime_tests PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    uml_add_unit_test(NAME gpu-vnc-lifetime
        COMMAND uml_gpu_vnc_lifetime_tests
        LABELS gpu)
endif()

get_property(_uml_registered_unit_binary_targets GLOBAL PROPERTY
    UML_UNIT_TEST_BINARY_TARGETS)
if(_uml_registered_unit_binary_targets)
    list(REMOVE_DUPLICATES _uml_registered_unit_binary_targets)
    add_dependencies(unit-test-binaries
        uml_instruction_evidence_test_runner
        ${_uml_registered_unit_binary_targets})
endif()
