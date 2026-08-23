# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT UML_APPLE_PLATFORM MATCHES "^ios" OR
   NOT UML_BUILD_KERNEL_RUNNER OR NOT UML_BUILD_GPU OR
   NOT UML_BUILD_AUDIO OR NOT UML_BUILD_NETWORK_PROVIDER)
    message(FATAL_ERROR
        "KernariumRuntime requires the iOS runner, GPU, audio, and network")
endif()

if(NOT UML_ACTIVE_KERNEL_ARTIFACT_TARGET OR
   NOT TARGET "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}")
    message(FATAL_ERROR
        "KernariumRuntime requires a validated prebuilt kernel artifact")
endif()
get_target_property(_kernarium_artifact_family
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}" UML_ARTIFACT_FAMILY)
get_target_property(_kernarium_kernel_image
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}" UML_KERNEL_IMAGE)
get_target_property(_kernarium_kernel_page_size
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}" UML_KERNEL_PAGE_SIZE)
get_target_property(_kernarium_kernel_release
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}" UML_KERNEL_RELEASE)
if(_kernarium_artifact_family STREQUAL "android-17-ack-6.18-16k")
    set(KERNARIUM_RUNTIME_ANDROID_GUEST 1)
elseif(_kernarium_artifact_family STREQUAL "mainline-6.18")
    set(KERNARIUM_RUNTIME_ANDROID_GUEST 0)
else()
    message(FATAL_ERROR
        "KernariumRuntime requires the mainline-6.18 or Android 17 artifact")
endif()
string(REGEX MATCH "^[0-9]+(\\.[0-9]+)+"
    KERNARIUM_KERNEL_RELEASE "${_kernarium_kernel_release}")
if(NOT KERNARIUM_KERNEL_RELEASE)
    message(FATAL_ERROR "Cannot derive the Kernarium release from the artifact")
endif()
string(REPLACE "." "_" KERNARIUM_KERNEL_RELEASE_COMPONENT
    "${KERNARIUM_KERNEL_RELEASE}")

if(UML_GUEST_16K_PAGES)
    set(KERNARIUM_KERNEL_PAGE_VARIANT 16K)
    set(_kernarium_guest_page_size 16384)
else()
    set(KERNARIUM_KERNEL_PAGE_VARIANT 4K)
    set(_kernarium_guest_page_size 4096)
endif()
if(NOT _kernarium_kernel_page_size EQUAL _kernarium_guest_page_size)
    message(FATAL_ERROR "Kernarium page profile differs from its kernel artifact")
endif()
# The application matches an embedded framework by name before it dares load
# one, so the guest flavour has to lead the identity rather than trail it: a
# trailing marker would leave the Android framework matching the Linux
# page-profile suffix as well.
if(KERNARIUM_RUNTIME_ANDROID_GUEST)
    if(NOT UML_GUEST_16K_PAGES)
        message(FATAL_ERROR
            "The Kernarium Android runtime supports only 16 KiB guest pages")
    endif()
    set(KERNARIUM_RUNTIME_FRAMEWORK_NAME
        "KernariumRuntimeAndroid_${KERNARIUM_KERNEL_RELEASE_COMPONENT}_${KERNARIUM_KERNEL_PAGE_VARIANT}")
else()
    set(KERNARIUM_RUNTIME_FRAMEWORK_NAME
        "KernariumRuntime_${KERNARIUM_KERNEL_RELEASE_COMPONENT}_${KERNARIUM_KERNEL_PAGE_VARIANT}")
endif()
string(REPLACE "_" "-" _kernarium_runtime_bundle_component
    "${KERNARIUM_RUNTIME_FRAMEWORK_NAME}")

find_program(_kernarium_python3 NAMES python3 REQUIRED)
execute_process(
    COMMAND "${_kernarium_python3}" -B
        "${PROJECT_SOURCE_DIR}/Kernarium/scripts/verify-signed-kernel-carrier.py"
        --image "${_kernarium_kernel_image}"
    RESULT_VARIABLE _kernarium_carrier_result
    OUTPUT_VARIABLE _kernarium_carrier_output
    ERROR_VARIABLE _kernarium_carrier_error)
if(NOT _kernarium_carrier_result EQUAL 0)
    message(FATAL_ERROR
        "Kernarium kernel is unsafe for the signed iOS carrier:\n"
        "${_kernarium_carrier_output}${_kernarium_carrier_error}")
endif()

set(_kernarium_cts_generator
    "${PROJECT_SOURCE_DIR}/Kernarium/scripts/generate-cts-campaign-header.py")
set(_kernarium_cts_runner
    "${PROJECT_SOURCE_DIR}/tests/integration/gpu/run_vk_gl_cts.py")
set(_kernarium_cts_identity
    "${PROJECT_SOURCE_DIR}/tests/integration/gpu/vk_gl_cts_identity.json")
set(_kernarium_cts_campaigns
    "${PROJECT_SOURCE_DIR}/tests/integration/gpu/vk_gl_cts_apple_campaigns.json")
set(_kernarium_cts_matrix
    "${PROJECT_SOURCE_DIR}/tests/integration/gpu/vk_gl_cts_apple_matrix.json")
set(_kernarium_cts_derivative_receipt
    "${PROJECT_SOURCE_DIR}/tests/integration/gpu/vk_gl_cts_derivative_rootfs.json")
set(_kernarium_cts_header
    "${CMAKE_CURRENT_BINARY_DIR}/KernariumCtsCampaigns.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_kernarium_cts_generator}"
    "${_kernarium_cts_runner}"
    "${_kernarium_cts_identity}"
    "${_kernarium_cts_campaigns}"
    "${_kernarium_cts_matrix}"
    "${_kernarium_cts_derivative_receipt}")
execute_process(
    COMMAND "${_kernarium_python3}" -B "${_kernarium_cts_generator}"
        --runner "${_kernarium_cts_runner}"
        --identity "${_kernarium_cts_identity}"
        --campaigns "${_kernarium_cts_campaigns}"
        --matrix "${_kernarium_cts_matrix}"
        --derivative-receipt "${_kernarium_cts_derivative_receipt}"
        --page-size "${_kernarium_guest_page_size}"
        --output "${_kernarium_cts_header}"
    RESULT_VARIABLE _kernarium_cts_result
    OUTPUT_VARIABLE _kernarium_cts_output
    ERROR_VARIABLE _kernarium_cts_error)
if(NOT _kernarium_cts_result EQUAL 0)
    message(FATAL_ERROR
        "Cannot generate Kernarium CTS campaigns:\n"
        "${_kernarium_cts_output}${_kernarium_cts_error}")
endif()

set(_kernarium_aquarium_generator
    "${PROJECT_SOURCE_DIR}/Kernarium/scripts/generate-aquarium-header.py")
set(_kernarium_aquarium_runner
    "${PROJECT_SOURCE_DIR}/tests/integration/gpu/run_chromium_webgl.py")
set(_kernarium_aquarium_header
    "${CMAKE_CURRENT_BINARY_DIR}/KernariumAquarium.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_kernarium_aquarium_generator}"
    "${_kernarium_aquarium_runner}")
execute_process(
    COMMAND "${_kernarium_python3}" -B "${_kernarium_aquarium_generator}"
        --runner "${_kernarium_aquarium_runner}"
        --output "${_kernarium_aquarium_header}"
    RESULT_VARIABLE _kernarium_aquarium_result
    OUTPUT_VARIABLE _kernarium_aquarium_output
    ERROR_VARIABLE _kernarium_aquarium_error)
if(NOT _kernarium_aquarium_result EQUAL 0)
    message(FATAL_ERROR
        "Cannot generate Kernarium Aquarium probe:\n"
        "${_kernarium_aquarium_output}${_kernarium_aquarium_error}")
endif()

add_library(KernariumRuntime SHARED
    "${_kernarium_aquarium_header}"
    "${_kernarium_cts_header}"
    KernariumRuntime.h
    KernariumRuntime.mm)
uml_declare_architecture_target(KernariumRuntime app)
target_include_directories(KernariumRuntime
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
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
if(UML_APPLE_PLATFORM STREQUAL "ios-simulator")
    set(_kernarium_aquarium_supported 1)
else()
    set(_kernarium_aquarium_supported 0)
endif()
target_compile_definitions(KernariumRuntime PRIVATE
    KERNARIUM_AQUARIUM_SUPPORTED=${_kernarium_aquarium_supported}
    KERNARIUM_KERNEL_RELEASE="${KERNARIUM_KERNEL_RELEASE}"
    KERNARIUM_RUNTIME_ANDROID_GUEST=${KERNARIUM_RUNTIME_ANDROID_GUEST}
    KERNARIUM_RUNTIME_FRAMEWORK_NAME="${KERNARIUM_RUNTIME_FRAMEWORK_NAME}")
if(KERNARIUM_RUNTIME_ANDROID_GUEST)
    get_target_property(_kernarium_android_manifest
        "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}" UML_ANDROID_RELEASE_MANIFEST)
    get_target_property(_kernarium_android_ramdisk
        "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}" UML_ANDROID_RAMDISK)
    get_target_property(_kernarium_android_base_disk
        "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}" UML_ANDROID_BASE_DISK)
    file(SIZE "${_kernarium_android_ramdisk}" _kernarium_android_ramdisk_size)
    file(SIZE "${_kernarium_android_base_disk}"
        _kernarium_android_base_disk_size)
    target_compile_definitions(KernariumRuntime PRIVATE
        KERNARIUM_ANDROID_RAMDISK_SIZE=${_kernarium_android_ramdisk_size}
        KERNARIUM_ANDROID_BASE_DISK_SIZE=${_kernarium_android_base_disk_size})
    # The accepted release identity travels inside the framework. A bundle
    # whose manifest differs is refused before any of its payload is opened.
    target_link_options(KernariumRuntime PRIVATE
        "LINKER:-sectcreate,__DATA,__uml_android,${_kernarium_android_manifest}")
    set_property(TARGET KernariumRuntime APPEND PROPERTY LINK_DEPENDS
        "${_kernarium_android_manifest}"
        "${_kernarium_android_ramdisk}"
        "${_kernarium_android_base_disk}")
endif()
target_link_options(KernariumRuntime PRIVATE
    "LINKER:-force_load,$<TARGET_FILE:uml_emulator>")
if(UML_APPLE_PLATFORM MATCHES "^ios")
    # The framework code signature covers __TEXT pages. The loader aliases
    # the ELF's immutable executable pages from this aligned carrier into the
    # kernel's runtime-selected native window instead of creating anonymous
    # code.
    target_link_options(KernariumRuntime PRIVATE
        "LINKER:-sectcreate,__TEXT,__uml_elf,${_kernarium_kernel_image}"
        "LINKER:-sectalign,__TEXT,__uml_elf,0x4000")
    set_property(TARGET KernariumRuntime APPEND PROPERTY
        LINK_DEPENDS "${_kernarium_kernel_image}")
endif()
add_dependencies(KernariumRuntime
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}"
    uml_dependency_prefix_verify)
add_dependencies(KernariumRuntime uml_ios_network_prefix_verify)
set_property(TARGET KernariumRuntime APPEND PROPERTY LINK_DEPENDS
    "${_kernarium_kernel_image}")
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
