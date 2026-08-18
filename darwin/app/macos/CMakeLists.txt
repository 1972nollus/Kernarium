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
    "${UML_DEPENDENCY_PREFIX}/share/uml-dependencies/receipt.tsv")
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
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
add_custom_command(TARGET uml-darwin POST_BUILD
    COMMAND ${PROJECT_SOURCE_DIR}/dependencies/verify-runtime-dependencies.sh
        --binary $<TARGET_FILE:uml-darwin>
    COMMENT "Checking production runtime dependency closure"
    VERBATIM)
