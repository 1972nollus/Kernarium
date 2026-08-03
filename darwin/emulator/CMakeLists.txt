# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_emulator STATIC
    src/emulator.cpp
    src/options.cpp
    ${UML_RUNTIME_CORE_OBJECTS}
    $<TARGET_OBJECTS:uml_network_device>)

add_library(UML::Emulator ALIAS uml_emulator)
uml_declare_architecture_target(uml_emulator emulator)
set_property(TARGET uml_emulator PROPERTY
    UML_ARCHITECTURE_AGGREGATE_PRODUCT TRUE)

target_link_libraries(uml_emulator PRIVATE
    UML::ApiContract
    UML::RuntimeContract)
target_include_directories(uml_emulator
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/darwin/api/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${PROJECT_SOURCE_DIR}/darwin/device/network
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu
        ${PROJECT_SOURCE_DIR}/darwin/runtime
        ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_emulator PUBLIC cxx_std_20)
target_compile_options(uml_emulator PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(uml_emulator PRIVATE
    UML_APPLE_PLATFORM_NAME="${UML_APPLE_PLATFORM}"
    UML_EMULATOR_HAS_KERNEL_RUNNER=$<BOOL:${UML_BUILD_KERNEL_RUNNER}>
    UML_EMULATOR_HAS_SLIRP=$<BOOL:${UML_BUILD_NETWORK_PROVIDER}>)

if(UML_BUILD_NETWORK_PROVIDER)
    target_link_libraries(uml_emulator PRIVATE UML::NetworkProvider)
endif()

set_target_properties(uml_emulator PROPERTIES
    OUTPUT_NAME uml_emulator
    POSITION_INDEPENDENT_CODE ON
    PUBLIC_HEADER
        "${PROJECT_SOURCE_DIR}/darwin/api/include/uml/emulator.h;${PROJECT_SOURCE_DIR}/darwin/api/include/uml/abi_identity.h")

if(UML_BUILD_KERNEL_RUNNER)
    target_sources(uml_emulator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/loader/elf_loader.cpp)
    target_include_directories(uml_emulator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/loader)
endif()

install(TARGETS uml_emulator
    ARCHIVE DESTINATION lib
    PUBLIC_HEADER DESTINATION include/uml)
