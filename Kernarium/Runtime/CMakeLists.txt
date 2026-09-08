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
if(_kernarium_artifact_family STREQUAL "android-6.18")
    set(KERNARIUM_RUNTIME_ANDROID_GUEST 1)
    set(KERNARIUM_RUNTIME_GUEST_FLAVOR
        KERNARIUM_RUNTIME_GUEST_ANDROID)
    set(KERNARIUM_KERNEL_RELEASE_SERIES "6.18")
elseif(_kernarium_artifact_family STREQUAL "mainline-6.18")
    set(KERNARIUM_RUNTIME_ANDROID_GUEST 0)
    if(UML_BUILD_WINE_ARM64EC)
        set(KERNARIUM_RUNTIME_GUEST_FLAVOR
            KERNARIUM_RUNTIME_GUEST_WINE_ARM64EC)
    else()
        set(KERNARIUM_RUNTIME_GUEST_FLAVOR
            KERNARIUM_RUNTIME_GUEST_LINUX)
    endif()
    set(KERNARIUM_KERNEL_RELEASE_SERIES "6.18")
else()
    message(FATAL_ERROR
        "KernariumRuntime requires the mainline-6.18 or Android 17 artifact")
endif()
string(REPLACE "." "_" KERNARIUM_KERNEL_RELEASE_COMPONENT
    "${KERNARIUM_KERNEL_RELEASE_SERIES}")

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
        "KRuntime_ACK_${KERNARIUM_KERNEL_RELEASE_COMPONENT}_${KERNARIUM_KERNEL_PAGE_VARIANT}")
elseif(UML_BUILD_WINE_ARM64EC)
    set(KERNARIUM_RUNTIME_FRAMEWORK_NAME
        "KRuntime_Wine_${KERNARIUM_KERNEL_RELEASE_COMPONENT}_${KERNARIUM_KERNEL_PAGE_VARIANT}")
else()
    set(KERNARIUM_RUNTIME_FRAMEWORK_NAME
        "KRuntime_${KERNARIUM_KERNEL_RELEASE_COMPONENT}_${KERNARIUM_KERNEL_PAGE_VARIANT}")
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
set(_kernarium_vulkan_cts_runner
    "${PROJECT_SOURCE_DIR}/tests/integration/gpu/run_vk_cts.py")
set(_kernarium_cts_identity
    "${PROJECT_SOURCE_DIR}/tests/integration/gpu/vk_gl_cts_identity.json")
set(_kernarium_vulkan_cts_identity
    "${PROJECT_SOURCE_DIR}/tests/integration/gpu/vulkan_cts_identity.json")
set(_kernarium_cts_campaigns
    "${PROJECT_SOURCE_DIR}/tests/integration/gpu/vk_gl_cts_apple_campaigns.json")
set(_kernarium_cts_matrix
    "${PROJECT_SOURCE_DIR}/tests/integration/gpu/vk_gl_cts_apple_matrix.json")
set(_kernarium_cts_derivative_receipt
    "${PROJECT_SOURCE_DIR}/tests/integration/gpu/vk_gl_cts_derivative_rootfs.json")
set(_kernarium_vulkan_cts_receipt
    "${PROJECT_SOURCE_DIR}/tests/integration/gpu/vulkan_cts_receipt.json")
set(_kernarium_vulkan_cts_mandatory
    "${PROJECT_SOURCE_DIR}/tests/integration/gpu/vulkan_cts_mandatory_1_4_6_2.txt")
set(_kernarium_cts_header
    "${CMAKE_CURRENT_BINARY_DIR}/KernariumCtsCampaigns.hpp")
set(_kernarium_vulkan_cts_options)
if(UML_APPLE_GPU_PRODUCT STREQUAL "apple-gpu-vulkan-experiment")
    list(APPEND _kernarium_vulkan_cts_options
        --experimental-venus
        --vulkan-receipt "${_kernarium_vulkan_cts_receipt}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_kernarium_vulkan_cts_receipt}"
        "${_kernarium_vulkan_cts_mandatory}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_kernarium_cts_generator}"
    "${_kernarium_cts_runner}"
    "${_kernarium_vulkan_cts_runner}"
    "${_kernarium_cts_identity}"
    "${_kernarium_vulkan_cts_identity}"
    "${_kernarium_cts_campaigns}"
    "${_kernarium_cts_matrix}"
    "${_kernarium_cts_derivative_receipt}")
execute_process(
    COMMAND "${_kernarium_python3}" -B "${_kernarium_cts_generator}"
        --runner "${_kernarium_cts_runner}"
        --vulkan-runner "${_kernarium_vulkan_cts_runner}"
        --identity "${_kernarium_cts_identity}"
        --vulkan-identity "${_kernarium_vulkan_cts_identity}"
        --campaigns "${_kernarium_cts_campaigns}"
        --matrix "${_kernarium_cts_matrix}"
        --derivative-receipt "${_kernarium_cts_derivative_receipt}"
        --page-size "${_kernarium_guest_page_size}"
        ${_kernarium_vulkan_cts_options}
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
        ${PROJECT_SOURCE_DIR}/darwin/device/battery
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${PROJECT_SOURCE_DIR}/darwin/device/input/virtio
        ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp)
if(UML_BUILD_WINE_ARM64EC)
    if(NOT TARGET UML::WineArm64ECEmulator)
        message(FATAL_ERROR "The ARM64EC Wine emulator target is unavailable")
    endif()
    set(_kernarium_emulator_target UML::WineArm64ECEmulator)
    set(_kernarium_emulator_archive_target uml_wine_arm64ec_emulator)
else()
    set(_kernarium_emulator_target UML::Emulator)
    set(_kernarium_emulator_archive_target uml_emulator)
endif()
target_link_libraries(KernariumRuntime PRIVATE
    ${_kernarium_emulator_target}
    UML::AndroidBoot
    UML::GuestPageConfiguration
    UML::AudioDevice
    UML::VideoDevice
    UML::BatteryDevice
    UML::GpuDevice
    UML::ConsoleDevice
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
    KERNARIUM_KERNEL_RELEASE_SERIES="${KERNARIUM_KERNEL_RELEASE_SERIES}"
    KERNARIUM_RUNTIME_ANDROID_GUEST=${KERNARIUM_RUNTIME_ANDROID_GUEST}
    KERNARIUM_RUNTIME_GUEST_FLAVOR=${KERNARIUM_RUNTIME_GUEST_FLAVOR}
    KERNARIUM_RUNTIME_FRAMEWORK_NAME="${KERNARIUM_RUNTIME_FRAMEWORK_NAME}")
target_link_options(KernariumRuntime PRIVATE
    "LINKER:-force_load,$<TARGET_FILE:${_kernarium_emulator_archive_target}>")
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
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "@loader_path/.."
    INSTALL_RPATH_USE_LINK_PATH FALSE
    MACOSX_FRAMEWORK_IDENTIFIER
        "com.imxieyi.${_kernarium_runtime_bundle_component}"
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/KernariumRuntime.h"
    XCODE_ATTRIBUTE_DEFINES_MODULE YES
    XCODE_ATTRIBUTE_PRODUCT_MODULE_NAME
        "${KERNARIUM_RUNTIME_FRAMEWORK_NAME}")

if(UML_BUILD_WINE_ARM64EC)
    add_custom_target(wine-arm64ec-ios-runtime DEPENDS KernariumRuntime)
endif()

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
