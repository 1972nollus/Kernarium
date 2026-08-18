# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT UML_APPLE_PLATFORM MATCHES "^ios" OR
   NOT UML_BUILD_KERNEL_RUNNER OR NOT UML_BUILD_GPU OR
   NOT UML_BUILD_AUDIO OR NOT UML_BUILD_NETWORK_PROVIDER)
    message(FATAL_ERROR
        "KernariumRuntime requires the iOS runner, GPU, audio, and network")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/kernel/patches/metadata.env"
    _kernarium_kernel_release_line
    REGEX "^UML_KERNEL_RELEASE_VERSION='[0-9]+(\\.[0-9]+)+'$")
list(LENGTH _kernarium_kernel_release_line _kernarium_kernel_release_count)
if(NOT _kernarium_kernel_release_count EQUAL 1)
    message(FATAL_ERROR
        "Cannot derive one Kernarium kernel release from metadata.env")
endif()
string(REGEX REPLACE
    "^UML_KERNEL_RELEASE_VERSION='([^']+)'$" "\\1"
    KERNARIUM_KERNEL_RELEASE "${_kernarium_kernel_release_line}")
string(REPLACE "." "_" KERNARIUM_KERNEL_RELEASE_COMPONENT
    "${KERNARIUM_KERNEL_RELEASE}")

if(UML_GUEST_16K_PAGES)
    set(KERNARIUM_KERNEL_PAGE_VARIANT 16K)
    set(_kernarium_kernel_image_default
        "/Volumes/Kernel/build-16k/vmlinux")
else()
    set(KERNARIUM_KERNEL_PAGE_VARIANT 4K)
    set(_kernarium_kernel_image_default
        "/Volumes/Kernel/build-4k/vmlinux")
endif()
set(KERNARIUM_RUNTIME_FRAMEWORK_NAME
    "KernariumRuntime_${KERNARIUM_KERNEL_RELEASE_COMPONENT}_${KERNARIUM_KERNEL_PAGE_VARIANT}")
string(REPLACE "_" "-" _kernarium_runtime_bundle_component
    "${KERNARIUM_RUNTIME_FRAMEWORK_NAME}")

set(UML_KERNARIUM_KERNEL_IMAGE "${_kernarium_kernel_image_default}"
    CACHE FILEPATH
        "Page-profile-specific AArch64 UML ET_DYN image embedded in Kernarium")
if(NOT EXISTS "${UML_KERNARIUM_KERNEL_IMAGE}")
    message(FATAL_ERROR
        "Kernarium kernel image does not exist: ${UML_KERNARIUM_KERNEL_IMAGE}")
endif()
find_program(_kernarium_python3 NAMES python3 REQUIRED)
execute_process(
    COMMAND "${_kernarium_python3}" -B
        "${PROJECT_SOURCE_DIR}/Kernarium/scripts/verify-signed-kernel-carrier.py"
        --image "${UML_KERNARIUM_KERNEL_IMAGE}"
    RESULT_VARIABLE _kernarium_carrier_result
    OUTPUT_VARIABLE _kernarium_carrier_output
    ERROR_VARIABLE _kernarium_carrier_error)
if(NOT _kernarium_carrier_result EQUAL 0)
    message(FATAL_ERROR
        "Kernarium kernel is unsafe for the signed iOS carrier:\n"
        "${_kernarium_carrier_output}${_kernarium_carrier_error}")
endif()

add_library(KernariumRuntime SHARED
    KernariumRuntime.h
    KernariumRuntime.mm)
uml_declare_architecture_target(KernariumRuntime app)
target_include_directories(KernariumRuntime
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/api/include
        ${PROJECT_SOURCE_DIR}/darwin/device
        ${PROJECT_SOURCE_DIR}/darwin/device/audio
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${PROJECT_SOURCE_DIR}/darwin/device/input/virtio
        ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp)
target_link_libraries(KernariumRuntime PRIVATE
    UML::Emulator
    UML::GuestPageConfiguration
    UML::AudioDevice
    UML::GpuDevice
    UML::VirtioInput
    UML::NetworkProvider
    UML::JitProfileConfiguration
    "-framework Foundation"
    "-framework Metal"
    "-framework MetalKit"
    "-framework UIKit")
target_compile_features(KernariumRuntime PRIVATE cxx_std_20)
target_compile_options(KernariumRuntime PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    -Wall -Wextra -Werror)
target_compile_definitions(KernariumRuntime PRIVATE
    KERNARIUM_KERNEL_RELEASE="${KERNARIUM_KERNEL_RELEASE}"
    KERNARIUM_RUNTIME_FRAMEWORK_NAME="${KERNARIUM_RUNTIME_FRAMEWORK_NAME}")
target_link_options(KernariumRuntime PRIVATE
    "LINKER:-force_load,$<TARGET_FILE:uml_emulator>")
if(UML_APPLE_PLATFORM MATCHES "^ios")
    # The framework code signature covers __TEXT pages. The loader aliases
    # the ELF's immutable executable pages from this aligned carrier into the
    # kernel's runtime-selected native window instead of creating anonymous
    # code.
    target_link_options(KernariumRuntime PRIVATE
        "LINKER:-sectcreate,__TEXT,__uml_elf,${UML_KERNARIUM_KERNEL_IMAGE}"
        "LINKER:-sectalign,__TEXT,__uml_elf,0x4000")
    set_property(TARGET KernariumRuntime APPEND PROPERTY
        LINK_DEPENDS "${UML_KERNARIUM_KERNEL_IMAGE}")
endif()
add_dependencies(KernariumRuntime uml_dependency_prefix_verify)
add_dependencies(KernariumRuntime uml_ios_network_prefix_verify)
set_property(TARGET KernariumRuntime APPEND PROPERTY LINK_DEPENDS
    "${UML_KERNARIUM_KERNEL_IMAGE}")
set_target_properties(KernariumRuntime PROPERTIES
    OUTPUT_NAME "${KERNARIUM_RUNTIME_FRAMEWORK_NAME}"
    FRAMEWORK TRUE
    FRAMEWORK_VERSION A
    MACOSX_FRAMEWORK_IDENTIFIER
        "com.imxieyi.${_kernarium_runtime_bundle_component}"
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/KernariumRuntime.h"
    XCODE_ATTRIBUTE_DEFINES_MODULE YES
    XCODE_ATTRIBUTE_PRODUCT_MODULE_NAME
        "${KERNARIUM_RUNTIME_FRAMEWORK_NAME}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/module.modulemap.in"
    "${CMAKE_CURRENT_BINARY_DIR}/module.modulemap"
    @ONLY)

add_custom_command(TARGET KernariumRuntime POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_BUNDLE_DIR:KernariumRuntime>/Modules"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/module.modulemap"
        "$<TARGET_BUNDLE_DIR:KernariumRuntime>/Modules/module.modulemap"
    COMMENT "Installing the versioned Kernarium runtime module map"
    VERBATIM)
