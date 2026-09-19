# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_test_allocation_failure STATIC allocation_failure.cpp)
target_include_directories(uml_test_allocation_failure PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_test_allocation_failure PUBLIC cxx_std_20)
target_compile_options(uml_test_allocation_failure PRIVATE
    -Wall -Wextra -Werror)

# CPU conformance and engine/runtime tests have their own directory scope.
# This keeps the suite entry point small while preserving one explicit source
# manifest per test domain.
add_custom_target(unit-test-binaries)
add_subdirectory(arm64)
if(TARGET uml_x86_64_gadgets)
    add_subdirectory(x86_64)
endif()
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

add_subdirectory(platform)
add_subdirectory(block)
add_subdirectory(sockets)
add_subdirectory(scratch)
add_subdirectory(mconsole)
add_subdirectory(input/vnc)
if(TARGET uml_virtio_input)
    add_subdirectory(input/virtio)
endif()
if(TARGET uml_audio_device)
    add_subdirectory(audio)
endif()
if(TARGET uml_battery_device)
    add_subdirectory(battery)
endif()
if(TARGET uml_console_device)
    add_subdirectory(console)
endif()
add_subdirectory(gpu)
if(TARGET uml_video_device)
    add_subdirectory(video)
endif()
