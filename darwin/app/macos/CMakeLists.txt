# SPDX-License-Identifier: GPL-2.0-or-later

function(uml_add_macos_embedded_kernel_app
        TARGET_NAME MAIN_SOURCE ARTIFACT_TARGET
        EMULATOR_TARGET EMULATOR_ARCHIVE_TARGET)
    if(NOT TARGET "${ARTIFACT_TARGET}")
        message(FATAL_ERROR
            "${TARGET_NAME} requires the artifact target ${ARTIFACT_TARGET}")
    endif()
    get_target_property(_uml_kernel_image "${ARTIFACT_TARGET}" UML_KERNEL_IMAGE)
    if(NOT _uml_kernel_image)
        message(FATAL_ERROR "${ARTIFACT_TARGET} has no kernel ELF image")
    endif()

    add_executable("${TARGET_NAME}"
        launcher.hpp
        main.cpp
        "${MAIN_SOURCE}")
    uml_declare_architecture_target("${TARGET_NAME}" app)
    target_include_directories("${TARGET_NAME}" PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/emulator/src
        ${PROJECT_SOURCE_DIR}/darwin/device)
    add_dependencies("${TARGET_NAME}"
        "${ARTIFACT_TARGET}"
        uml_dependency_prefix_verify)
    set_property(TARGET "${TARGET_NAME}" APPEND PROPERTY LINK_DEPENDS
        "${_uml_kernel_image}"
        "${PROJECT_SOURCE_DIR}/dependencies/verify-runtime-dependencies.sh"
        "${UML_DEPENDENCY_PREFIX}/share/uml-dependencies/receipt.tsv"
        "${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/EGL.framework/EGL"
        "${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/EGL.framework/Info.plist"
        "${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/GLESv2.framework/GLESv2"
        "${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/GLESv2.framework/Info.plist")
    set(_uml_kosmickrisp_stage_commands)
    if(TARGET uml_third_party_kosmickrisp)
        set_property(TARGET "${TARGET_NAME}" APPEND PROPERTY LINK_DEPENDS
            "${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}/KosmicKrisp"
            "${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}/Info.plist")
        list(APPEND _uml_kosmickrisp_stage_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}
                $<TARGET_FILE_DIR:${TARGET_NAME}>/Frameworks/KosmicKrisp.framework)
    endif()
    target_link_libraries("${TARGET_NAME}" PRIVATE
        "${EMULATOR_TARGET}"
        UML::AudioDevice
        UML::BatteryDevice
        UML::VideoDevice
        UML::ConsoleDevice
        UML::GpuDevice
        UML::VirtioInput
        UML::JitProfileConfiguration)
    if(TARGET uml_network_provider)
        target_link_libraries("${TARGET_NAME}" PRIVATE UML::NetworkProvider)
    endif()
    target_compile_definitions("${TARGET_NAME}" PRIVATE
        UML_APP_HAS_SLIRP=$<BOOL:${UML_BUILD_NETWORK_PROVIDER}>)
    target_compile_options("${TARGET_NAME}" PRIVATE -Wall -Wextra -Werror)
    target_link_options("${TARGET_NAME}" PRIVATE
        "LINKER:-export_dynamic"
        # The embedded ELF resolves this fixed-arity ABI with dlsym(). These
        # symbols have no native Mach-O callers, so force-load the deliberately
        # aggregated emulator product at the application boundary.
        "LINKER:-force_load,$<TARGET_FILE:${EMULATOR_ARCHIVE_TARGET}>"
        "LINKER:-sectcreate,__DATA,__uml_elf,${_uml_kernel_image}")

    set_target_properties("${TARGET_NAME}" PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/Frameworks"
        INSTALL_RPATH_USE_LINK_PATH FALSE)
    add_custom_command(TARGET "${TARGET_NAME}" POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            $<TARGET_FILE_DIR:${TARGET_NAME}>/Frameworks
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:${TARGET_NAME}>/Frameworks
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/EGL.framework
            $<TARGET_FILE_DIR:${TARGET_NAME}>/Frameworks/EGL.framework
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/GLESv2.framework
            $<TARGET_FILE_DIR:${TARGET_NAME}>/Frameworks/GLESv2.framework
        ${_uml_kosmickrisp_stage_commands}
        COMMAND ${PROJECT_SOURCE_DIR}/dependencies/verify-runtime-dependencies.sh
            --binary $<TARGET_FILE:${TARGET_NAME}>
            --apple-gpu-prefix ${UML_APPLE_GPU_DEPENDENCY_PREFIX}
            --apple-gpu-product ${UML_APPLE_GPU_PRODUCT}
            --framework-directory $<TARGET_FILE_DIR:${TARGET_NAME}>/Frameworks
        COMMENT "Staging and checking the ${TARGET_NAME} Apple GPU runtime closure"
        VERBATIM)
endfunction()

if(UML_BUILD_CLI)
    if(NOT UML_ACTIVE_KERNEL_ARTIFACT_TARGET)
        message(FATAL_ERROR "uml-darwin requires a selected mainline artifact")
    endif()
    if(UML_BUILD_WINE_ARM64EC)
        if(NOT TARGET UML::WineArm64ECEmulator)
            message(FATAL_ERROR
                "The ARM64EC Wine macOS app requires its emulator archive")
        endif()
        uml_add_macos_embedded_kernel_app(
            uml-darwin-wine-arm64ec linux_main.cpp
            "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}"
            UML::WineArm64ECEmulator uml_wine_arm64ec_emulator)
        target_compile_definitions(uml-darwin-wine-arm64ec PRIVATE
            UML_APP_TOUCHSCREEN_INPUT=1)
    else()
        uml_add_macos_embedded_kernel_app(
            uml-darwin linux_main.cpp "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}"
            UML::Emulator uml_emulator)
    endif()
endif()

if(UML_BUILD_ANDROID17_CLI)
    if(NOT UML_ACTIVE_KERNEL_ARTIFACT_TARGET OR
       NOT TARGET "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}")
        message(FATAL_ERROR
            "uml-darwin-android17 requires a compatible ACK artifact")
    endif()

    uml_add_macos_embedded_kernel_app(
        uml-darwin-android17 android17_main.cpp
        "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}"
        UML::Emulator uml_emulator)
    target_link_libraries(uml-darwin-android17 PRIVATE UML::AndroidBoot)
endif()
