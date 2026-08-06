# SPDX-License-Identifier: GPL-2.0-or-later

set(_uml_emulator_boundary_sources
    src/emulator.cpp
    src/options.cpp)
if(UML_BUILD_KERNEL_RUNNER)
    list(APPEND _uml_emulator_boundary_sources loader/elf_loader.cpp)
endif()

function(_uml_add_emulator_boundary TARGET ADMISSION_TESTING)
    add_library(${TARGET} OBJECT ${_uml_emulator_boundary_sources})
    uml_declare_architecture_target(${TARGET} emulator)
    target_link_libraries(${TARGET} PRIVATE
        UML::ApiContract
        UML::GuestPageConfiguration
        UML::RuntimeContract)
    target_include_directories(${TARGET} PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/api/include
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/device/network
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu
        ${PROJECT_SOURCE_DIR}/darwin/runtime
        ${CMAKE_CURRENT_SOURCE_DIR})
    if(UML_BUILD_KERNEL_RUNNER)
        target_include_directories(${TARGET} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/loader)
    endif()
    target_compile_features(${TARGET} PUBLIC cxx_std_20)
    target_compile_options(${TARGET} PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(${TARGET} PRIVATE
        UML_APPLE_PLATFORM_NAME="${UML_APPLE_PLATFORM}"
        UML_EMULATOR_HAS_KERNEL_RUNNER=$<BOOL:${UML_BUILD_KERNEL_RUNNER}>
        UML_EMULATOR_ADMISSION_TESTING=${ADMISSION_TESTING})
    set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

_uml_add_emulator_boundary(uml_emulator_boundary 0)

add_library(uml_emulator STATIC
    $<TARGET_OBJECTS:uml_emulator_boundary>)

add_library(UML::Emulator ALIAS uml_emulator)
uml_declare_architecture_target(uml_emulator emulator)
set_property(TARGET uml_emulator PROPERTY
    UML_ARCHITECTURE_AGGREGATE_PRODUCT TRUE)

target_link_libraries(uml_emulator PRIVATE
    UML::ApiContract
    UML::GuestPageConfiguration
    UML::NetworkDevice
    UML::RuntimeCore
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

set_target_properties(uml_emulator PROPERTIES
    OUTPUT_NAME uml_emulator
    POSITION_INDEPENDENT_CODE ON
    PUBLIC_HEADER
        "${PROJECT_SOURCE_DIR}/darwin/api/include/uml/emulator.h;${PROJECT_SOURCE_DIR}/darwin/api/include/uml/abi_identity.h")

set(_uml_need_admission_test_variant OFF)
if(UML_BUILD_UNIT_TESTS OR
   (UML_BUILD_TOOLING_TESTS AND UML_BUILD_KERNEL_RUNNER))
    set(_uml_need_admission_test_variant ON)
endif()
if(_uml_need_admission_test_variant)
    # Deterministic admission interleavings and memory failure probes belong
    # to a test-only product variant. The emulator and memory modules reuse
    # the exact production source manifests under compile-time test policies.
    _uml_add_emulator_boundary(uml_emulator_boundary_test_variant 1)
    add_library(uml_emulator_admission_test_variant STATIC
        $<TARGET_OBJECTS:uml_emulator_boundary_test_variant>)
    uml_declare_architecture_target(
        uml_emulator_admission_test_variant emulator)
    set_property(TARGET uml_emulator_admission_test_variant PROPERTY
        UML_ARCHITECTURE_AGGREGATE_PRODUCT TRUE)
    set(_uml_test_runtime_core UML::RuntimeCore)
    if(TARGET uml_runtime_core_test_variant)
        set(_uml_test_runtime_core UML::RuntimeCoreTestVariant)
        target_compile_definitions(uml_emulator_admission_test_variant
            PUBLIC UML_MEMORY_ALLOCATION_TESTING=1)
    endif()
    target_link_libraries(uml_emulator_admission_test_variant PRIVATE
        UML::ApiContract
        UML::GuestPageConfiguration
        UML::NetworkDevice
        ${_uml_test_runtime_core}
        UML::RuntimeContract)
    target_include_directories(uml_emulator_admission_test_variant
        PUBLIC ${PROJECT_SOURCE_DIR}/darwin/api/include
        PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin/include
            ${PROJECT_SOURCE_DIR}/darwin/device/network
            ${PROJECT_SOURCE_DIR}/darwin/memory/mmu
            ${PROJECT_SOURCE_DIR}/darwin/runtime
            ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(uml_emulator_admission_test_variant PUBLIC
        cxx_std_20)
    set_target_properties(uml_emulator_admission_test_variant PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
endif()

install(TARGETS uml_emulator
    ARCHIVE DESTINATION lib
    PUBLIC_HEADER DESTINATION include/uml)

# A static C API implemented in C++ must carry its runtime link requirement to
# installed C consumers. Keep this small config relocatable and describe the
# exact platform/page/profile represented by the archive beside it.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/UMLEmulatorConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/UMLEmulatorConfig.cmake
    @ONLY NEWLINE_STYLE UNIX)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/UMLEmulatorConfig.cmake
    DESTINATION lib/cmake/UMLEmulator)
