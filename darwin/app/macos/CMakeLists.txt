# SPDX-License-Identifier: GPL-2.0-or-later

# Application composition belongs here; the foreign Kbuild policy and its
# generated image are owned by kernel/CMakeLists.txt.
if(NOT TARGET uml_kernel OR NOT UML_KERNEL_IMAGE)
    message(FATAL_ERROR "The macOS application requires the kernel module")
endif()

add_executable(uml-darwin main.cpp)
uml_declare_architecture_target(uml-darwin app)
target_include_directories(uml-darwin PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/emulator/src
    ${PROJECT_SOURCE_DIR}/darwin/device)
add_dependencies(uml-darwin uml_kernel uml_dependency_prefix_verify)
set_property(TARGET uml-darwin APPEND PROPERTY LINK_DEPENDS
    "${UML_KERNEL_IMAGE}"
    "${PROJECT_SOURCE_DIR}/dependencies/verify-runtime-dependencies.sh"
    "${UML_DEPENDENCY_PREFIX}/share/uml-dependencies/receipt.tsv"
    "${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/EGL.framework/EGL"
    "${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/EGL.framework/Info.plist"
    "${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/GLESv2.framework/GLESv2"
    "${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/GLESv2.framework/Info.plist")
target_link_libraries(uml-darwin PRIVATE
    UML::Emulator
    UML::AudioDevice
    UML::GpuDevice
    UML::JitProfileConfiguration)
if(TARGET uml_network_provider)
    target_link_libraries(uml-darwin PRIVATE UML::NetworkProvider)
endif()
target_compile_definitions(uml-darwin PRIVATE
    UML_APP_HAS_SLIRP=$<BOOL:${UML_BUILD_NETWORK_PROVIDER}>)
target_compile_options(uml-darwin PRIVATE -Wall -Wextra -Werror)
target_link_options(uml-darwin PRIVATE
    "LINKER:-export_dynamic"
    # The embedded ELF resolves this fixed-arity ABI with dlsym(). These
    # symbols have no native Mach-O callers, so force-load the deliberately
    # aggregated emulator product at the application boundary.
    "LINKER:-force_load,$<TARGET_FILE:uml_emulator>"
    "LINKER:-sectcreate,__DATA,__uml_elf,${UML_KERNEL_IMAGE}")

set_target_properties(uml-darwin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "@executable_path/Frameworks"
    INSTALL_RPATH_USE_LINK_PATH FALSE)
add_custom_command(TARGET uml-darwin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        $<TARGET_FILE_DIR:uml-darwin>/Frameworks
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:uml-darwin>/Frameworks
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/EGL.framework
        $<TARGET_FILE_DIR:uml-darwin>/Frameworks/EGL.framework
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/GLESv2.framework
        $<TARGET_FILE_DIR:uml-darwin>/Frameworks/GLESv2.framework
    COMMAND ${PROJECT_SOURCE_DIR}/dependencies/verify-runtime-dependencies.sh
        --binary $<TARGET_FILE:uml-darwin>
        --apple-gpu-prefix ${UML_APPLE_GPU_DEPENDENCY_PREFIX}
        --framework-directory $<TARGET_FILE_DIR:uml-darwin>/Frameworks
    COMMENT "Staging and checking the production Apple GPU runtime closure"
    VERBATIM)
