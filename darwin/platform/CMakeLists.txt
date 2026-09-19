# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_platform_contract INTERFACE)
add_library(UML::PlatformContract ALIAS uml_platform_contract)
uml_declare_architecture_target(uml_platform_contract platform)
target_compile_features(uml_platform_contract INTERFACE cxx_std_20)

add_library(uml_video_decoder STATIC apple/video_decoder.mm)
add_library(UML::VideoDecoder ALIAS uml_video_decoder)
uml_declare_architecture_target(uml_video_decoder platform)
target_include_directories(uml_video_decoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_video_decoder PUBLIC cxx_std_20)
set_target_properties(uml_video_decoder PROPERTIES OBJCXX_STANDARD 20)
target_compile_options(uml_video_decoder PRIVATE -Wall -Wextra -Werror -fobjc-arc)
target_link_libraries(uml_video_decoder PRIVATE UML::PlatformContract
    "-framework Foundation" "-framework CoreMedia" "-framework CoreVideo"
    "-framework VideoToolbox")

add_library(uml_host_battery STATIC apple/host_battery.mm)
add_library(UML::HostBattery ALIAS uml_host_battery)
uml_declare_architecture_target(uml_host_battery platform)
target_include_directories(uml_host_battery PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_host_battery PUBLIC cxx_std_20)
set_target_properties(uml_host_battery PROPERTIES OBJCXX_STANDARD 20)
target_compile_options(uml_host_battery PRIVATE
    -Wall -Wextra -Werror
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(uml_host_battery PRIVATE
    UML::PlatformContract
    "-framework Foundation")
if(UML_TARGET_MACOS)
    target_link_libraries(uml_host_battery PRIVATE "-framework IOKit")
else()
    target_link_libraries(uml_host_battery PRIVATE "-framework UIKit")
endif()

add_library(uml_run_routing OBJECT run_routing.cpp)
add_library(UML::RunRouting ALIAS uml_run_routing)
uml_declare_architecture_target(uml_run_routing platform)
target_include_directories(uml_run_routing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_run_routing PUBLIC cxx_std_20)
target_compile_options(uml_run_routing PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(uml_run_routing PRIVATE
    UML_RUN_ROUTING_TESTING=0)
set_target_properties(uml_run_routing PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_host_topology OBJECT host_topology.cpp)
add_library(UML::HostTopology ALIAS uml_host_topology)
uml_declare_architecture_target(uml_host_topology platform)
target_link_libraries(uml_host_topology PRIVATE UML::PlatformContract)
target_include_directories(uml_host_topology PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_host_topology PUBLIC cxx_std_20)
target_compile_options(uml_host_topology PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_host_topology PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_host_environment OBJECT apple/host_environment.mm)
add_library(UML::HostEnvironment ALIAS uml_host_environment)
uml_declare_architecture_target(uml_host_environment platform)
target_link_libraries(uml_host_environment PRIVATE
    UML::PlatformContract)
target_include_directories(uml_host_environment PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_host_environment PUBLIC cxx_std_20)
set_target_properties(uml_host_environment PROPERTIES
    OBJCXX_STANDARD 20
    POSITION_INDEPENDENT_CODE ON)
target_compile_options(uml_host_environment PRIVATE
    -Wall -Wextra -Werror
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

add_library(uml_guest_files OBJECT guest_files.cpp)
add_library(UML::GuestFiles ALIAS uml_guest_files)
uml_declare_architecture_target(uml_guest_files platform)
target_link_libraries(uml_guest_files PRIVATE UML::PlatformContract)
target_include_directories(uml_guest_files PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_guest_files PUBLIC cxx_std_20)
target_compile_options(uml_guest_files PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_guest_files PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_platform_component INTERFACE)
add_library(UML::PlatformComponent ALIAS uml_platform_component)
uml_declare_architecture_target(uml_platform_component platform)
target_link_libraries(uml_platform_component INTERFACE
    UML::PlatformContract
    "-framework Foundation")
target_sources(uml_platform_component INTERFACE
    $<TARGET_OBJECTS:uml_run_routing>
    $<TARGET_OBJECTS:uml_host_topology>
    $<TARGET_OBJECTS:uml_host_environment>
    $<TARGET_OBJECTS:uml_guest_files>)
