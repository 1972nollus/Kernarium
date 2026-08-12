# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT UML_APPLE_PLATFORM STREQUAL "ios-simulator" OR
   NOT UML_BUILD_KERNEL_RUNNER OR NOT UML_BUILD_GPU)
    message(FATAL_ERROR
        "KernariumRuntime requires the iOS Simulator kernel runner and GPU")
endif()

set(UML_KERNARIUM_KERNEL_IMAGE
    "${PROJECT_SOURCE_DIR}/.build/kernel/linux"
    CACHE FILEPATH "AArch64 UML ET_DYN image embedded in KernariumRuntime")
if(NOT EXISTS "${UML_KERNARIUM_KERNEL_IMAGE}")
    message(FATAL_ERROR
        "Kernarium kernel image does not exist: ${UML_KERNARIUM_KERNEL_IMAGE}")
endif()

add_library(KernariumRuntime SHARED
    KernariumRuntime.h
    KernariumRuntime.mm)
uml_declare_architecture_target(KernariumRuntime app)
target_include_directories(KernariumRuntime
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/api/include
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
target_link_libraries(KernariumRuntime PRIVATE
    UML::Emulator
    UML::GpuDevice
    UML::JitProfileConfiguration
    "-framework Foundation"
    "-framework Metal"
    "-framework MetalKit"
    "-framework UIKit")
target_compile_features(KernariumRuntime PRIVATE cxx_std_20)
target_compile_options(KernariumRuntime PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    -Wall -Wextra -Werror)
target_link_options(KernariumRuntime PRIVATE
    "LINKER:-force_load,$<TARGET_FILE:uml_emulator>"
    "LINKER:-sectcreate,__DATA,__uml_elf,${UML_KERNARIUM_KERNEL_IMAGE}")
add_dependencies(KernariumRuntime uml_dependency_prefix_verify)
set_property(TARGET KernariumRuntime APPEND PROPERTY LINK_DEPENDS
    "${UML_KERNARIUM_KERNEL_IMAGE}")
set_target_properties(KernariumRuntime PROPERTIES
    FRAMEWORK TRUE
    FRAMEWORK_VERSION A
    MACOSX_FRAMEWORK_IDENTIFIER com.imxieyi.KernariumRuntime
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/KernariumRuntime.h"
    XCODE_ATTRIBUTE_DEFINES_MODULE YES)

add_custom_command(TARGET KernariumRuntime POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_BUNDLE_DIR:KernariumRuntime>/Modules"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/module.modulemap"
        "$<TARGET_BUNDLE_DIR:KernariumRuntime>/Modules/module.modulemap"
    COMMENT "Installing the KernariumRuntime Swift module map"
    VERBATIM)
