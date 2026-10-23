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
# The import verified the artifact as release series 6.18 with 4 KiB pages.
if(_kernarium_artifact_family STREQUAL "android-6.18")
    set(KERNARIUM_RUNTIME_ANDROID_GUEST 1)
    set(KERNARIUM_RUNTIME_GUEST_FLAVOR
        KERNARIUM_RUNTIME_GUEST_ANDROID)
elseif(_kernarium_artifact_family STREQUAL "mainline-6.18")
    set(KERNARIUM_RUNTIME_ANDROID_GUEST 0)
    set(KERNARIUM_RUNTIME_GUEST_FLAVOR
        KERNARIUM_RUNTIME_GUEST_LINUX)
else()
    message(FATAL_ERROR
        "KernariumRuntime requires the mainline-6.18 or Android 17 artifact")
endif()
set(KERNARIUM_KERNEL_RELEASE_COMPONENT 6_18)
set(KERNARIUM_KERNEL_PAGE_VARIANT 4K)
# The application matches an embedded framework by name before it dares load
# one, so the guest flavour has to lead the identity rather than trail it: a
# trailing marker would leave the Android framework matching the Linux
# page-profile suffix as well.
if(KERNARIUM_RUNTIME_ANDROID_GUEST)
    set(KERNARIUM_RUNTIME_FRAMEWORK_NAME
        "KRuntime_ACK_${KERNARIUM_KERNEL_RELEASE_COMPONENT}_${KERNARIUM_KERNEL_PAGE_VARIANT}")
else()
    set(KERNARIUM_RUNTIME_FRAMEWORK_NAME
        "KRuntime_${KERNARIUM_KERNEL_RELEASE_COMPONENT}_${KERNARIUM_KERNEL_PAGE_VARIANT}")
endif()
string(REPLACE "_" "-" _kernarium_runtime_bundle_component
    "${KERNARIUM_RUNTIME_FRAMEWORK_NAME}")

execute_process(
    COMMAND "${Python3_EXECUTABLE}" -B
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

add_library(KernariumRuntime SHARED
    KernariumConsoleInput.hpp
    KernariumRuntime.h
    KernariumRuntime.mm)
uml_declare_architecture_target(KernariumRuntime app)

# The release runtime boots only the desktop. The automation runtime, which
# only test and benchmark drivers package, also accepts a host-built boot
# payload and relays a guest campaign's artifacts; Kernarium/README.md owns
# that contract.
if(UML_KERNARIUM_AUTOMATION_HARNESS)
    if(KERNARIUM_RUNTIME_ANDROID_GUEST)
        message(FATAL_ERROR
            "The Kernarium automation harness boots Linux guests only")
    endif()
    target_sources(KernariumRuntime PRIVATE
        KernariumHarness.hpp
        KernariumHarness.mm
        KernariumPayloadFile.hpp)
    set(_kernarium_automation_harness 1)
else()
    set(_kernarium_automation_harness 0)
endif()
target_include_directories(KernariumRuntime
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/api/include
        ${PROJECT_SOURCE_DIR}/darwin/device
        ${PROJECT_SOURCE_DIR}/darwin/device/audio
        ${PROJECT_SOURCE_DIR}/darwin/device/battery
        ${PROJECT_SOURCE_DIR}/darwin/device/balloon
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu
        ${PROJECT_SOURCE_DIR}/darwin/device/input/virtio
        ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp)
target_link_libraries(KernariumRuntime PRIVATE
    UML::Emulator
    UML::AndroidBoot
    UML::BootInitramfs
    UML::GuestPageConfiguration
    UML::AudioDevice
    UML::VideoDevice
    UML::BatteryDevice
    UML::BalloonDevice
    UML::GpuDevice
    UML::ConsoleDevice
    UML::VirtioInput
    UML::NetworkProvider
    "-framework Foundation"
    "-framework Metal"
    "-framework MetalKit"
    "-framework UIKit")
target_compile_definitions(KernariumRuntime PRIVATE
    KERNARIUM_RUNTIME_ANDROID_GUEST=${KERNARIUM_RUNTIME_ANDROID_GUEST}
    KERNARIUM_RUNTIME_GUEST_FLAVOR=${KERNARIUM_RUNTIME_GUEST_FLAVOR}
    KERNARIUM_AUTOMATION_HARNESS=${_kernarium_automation_harness})
target_link_options(KernariumRuntime PRIVATE
    "LINKER:-force_load,$<TARGET_FILE:uml_emulator>")
# The framework code signature covers __TEXT pages. The loader aliases the
# ELF's immutable executable pages from this aligned carrier into the
# kernel's runtime-selected native window instead of creating anonymous code.
target_link_options(KernariumRuntime PRIVATE
    "LINKER:-sectcreate,__TEXT,__uml_elf,${_kernarium_kernel_image}"
    "LINKER:-sectalign,__TEXT,__uml_elf,0x4000")
# A Linux guest boots the accepted aarch64 base archive and the generic
# per-run /init, carried beside the kernel as uml-darwin carries them. Android
# boots its own ramdisk.
if(NOT KERNARIUM_RUNTIME_ANDROID_GUEST)
    include("${PROJECT_SOURCE_DIR}/tools/rootfs/boot-ramdisk/BootBase.cmake")
    uml_select_boot_base(aarch64 _kernarium_boot_base)
    set(_kernarium_boot_init
        "${PROJECT_SOURCE_DIR}/tools/rootfs/boot-ramdisk/init")
    target_link_options(KernariumRuntime PRIVATE
        "LINKER:-sectcreate,__TEXT,__uml_boot_base,${_kernarium_boot_base}"
        "LINKER:-sectcreate,__TEXT,__uml_boot_init,${_kernarium_boot_init}")
    set_property(TARGET KernariumRuntime APPEND PROPERTY LINK_DEPENDS
        "${_kernarium_boot_base}" "${_kernarium_boot_init}")
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
