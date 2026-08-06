# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_platform_contract INTERFACE)
add_library(UML::PlatformContract ALIAS uml_platform_contract)
uml_declare_architecture_target(uml_platform_contract platform)
target_compile_features(uml_platform_contract INTERFACE cxx_std_20)

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

add_library(uml_platform_component INTERFACE)
add_library(UML::PlatformComponent ALIAS uml_platform_component)
uml_declare_architecture_target(uml_platform_component platform)
target_link_libraries(uml_platform_component INTERFACE
    UML::PlatformContract)
target_sources(uml_platform_component INTERFACE
    $<TARGET_OBJECTS:uml_run_routing>)
