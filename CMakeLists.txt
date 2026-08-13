cmake_minimum_required(VERSION 3.27)

project(uml_apple LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(UmlTesting)
include(VerifyDependencies)
include(VerifyAbi)
include(UmlArchitecture)

uml_verify_generated_abi()

# Request the codemodel for this configure invocation. Unlike creating a
# query file while configuration is already in progress, cmake_file_api()
# guarantees that a fresh build tree has its reply at generation time.
cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2)

set(UML_APPLE_PLATFORM "macos" CACHE STRING
    "Apple target platform: macos, ios, or ios-simulator")
set_property(CACHE UML_APPLE_PLATFORM PROPERTY STRINGS macos ios ios-simulator)

if(NOT UML_APPLE_PLATFORM MATCHES "^(macos|ios|ios-simulator)$")
    message(FATAL_ERROR "Unsupported UML_APPLE_PLATFORM=${UML_APPLE_PLATFORM}")
endif()

if(NOT APPLE)
    message(FATAL_ERROR "The UML Apple emulator requires an Apple SDK")
endif()

if(UML_APPLE_PLATFORM STREQUAL "macos")
    set(UML_TARGET_MACOS ON)
    option(UML_BUILD_CLI "Build the arm64 macOS command-line app" ON)
    option(UML_BUILD_KERNEL_RUNNER
           "Build the embedded ELF kernel runner for the macOS CLI" ON)
elseif(UML_APPLE_PLATFORM MATCHES "^ios")
    set(UML_TARGET_MACOS OFF)
    option(UML_BUILD_CLI "Build the arm64 macOS command-line app" OFF)
    option(UML_BUILD_KERNEL_RUNNER
        "Build the prototype embedded ELF runner for Kernarium on iOS" OFF)
endif()

option(UML_BUILD_UNIT_TESTS
    "Build host-only C++/assembly unit tests"
    ${UML_TARGET_MACOS})
option(UML_BUILD_INTEGRATION_TESTS
    "Register guest, rootfs, Python, VNC, and workload integration tests" OFF)
set(_uml_build_tooling_tests_default OFF)
if(UML_BUILD_UNIT_TESTS OR UML_BUILD_INTEGRATION_TESTS)
    set(_uml_build_tooling_tests_default ON)
endif()
option(UML_BUILD_TOOLING_TESTS
    "Build and register host-only repository/tooling validation tests"
    ${_uml_build_tooling_tests_default})
set(UML_INTEGRATION_ROOTFS "" CACHE FILEPATH
    "Unified EROFS rootfs image used by integration tests")
set(UML_INSTRUCTION_MATERIALIZATION_WORK_ROOT "" CACHE PATH
    "Optional external root for large instruction-materialization join state")
# Every unit test emits one compressed input-materialization sidecar when this
# is on, and only the instruction-materialization join consumes them. A
# complete 16 KiB profile costs roughly 6.6 GiB of compressed sidecars against
# 3.4 MiB of ordinary evidence, so ordinary unit and tooling runs leave it off
# and the evidence campaign turns it on explicitly.
option(UML_INSTRUCTION_INPUT_MATERIALIZATION
    "Record per-test instruction input materializations and register their join"
    OFF)
option(UML_ENABLE_EXTERNAL_NETWORK_TESTS
    "Register guest tests that intentionally access the public network" OFF)
set(UML_EXTERNAL_NETWORK_TEST_HOST "example.com" CACHE STRING
    "Public host used only when UML_ENABLE_EXTERNAL_NETWORK_TESTS is enabled")
set(UML_DEPENDENCY_PREFIX
    "${PROJECT_SOURCE_DIR}/.build/dependencies/macos-arm64"
    CACHE PATH "Private prefix containing source-built host dependencies")
option(UML_BUILD_GPU "Build the host GPU/display provider" ${UML_TARGET_MACOS})
option(UML_BUILD_KERNARIUM_RUNTIME
    "Build the Kernarium application framework for iOS" OFF)
option(UML_BUILD_NETWORK_PROVIDER
    "Build the optional SLiRP network provider"
    ${UML_BUILD_KERNEL_RUNNER})

if(UML_BUILD_GPU)
    # Both native display implementations are Objective-C++. Keep that
    # language out of ordinary CPU/library-only configurations.
    enable_language(OBJCXX)
endif()

option(UML_GUEST_16K_PAGES
    "Build the kernel and static userspace MMU for 16 KiB guest pages" ON)
option(DARWIN_UML_JIT_GPR_LINK_PROFILE
    "Instrument successful JIT links for guest-GPR traffic profiling" OFF)
option(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE
    "Instrument JIT translation for resident-GPR pressure profiling" OFF)
option(UML_JIT_SHARE_TRANSLATIONS_BETWEEN_EXECUTORS
    "Share relocatable JIT translations between concurrent executors" ON)
option(UML_JIT_CARRY_X8_X9_ACROSS_LINKS
    "Keep guest x8/x9 resident across successful JIT block links" OFF)
option(UML_ENABLE_RUNTIME_PROFILING
    "Compile optional runtime, MMU, and device profiling into products" OFF)

if(UML_JIT_CARRY_X8_X9_ACROSS_LINKS AND
   DARWIN_UML_JIT_GPR_LINK_PROFILE)
    message(FATAL_ERROR
        "UML_JIT_CARRY_X8_X9_ACROSS_LINKS cannot be combined with "
        "DARWIN_UML_JIT_GPR_LINK_PROFILE")
endif()

if(UML_ENABLE_RUNTIME_PROFILING OR
   DARWIN_UML_JIT_GPR_LINK_PROFILE OR
   DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
    set(UML_ENABLE_JIT_PROFILE ON)
else()
    set(UML_ENABLE_JIT_PROFILE OFF)
endif()

add_library(uml_jit_profile_configuration INTERFACE)
add_library(UML::JitProfileConfiguration ALIAS
    uml_jit_profile_configuration)
target_compile_definitions(uml_jit_profile_configuration INTERFACE
    UML_ENABLE_JIT_PROFILE=$<BOOL:${UML_ENABLE_JIT_PROFILE}>
    UML_JIT_SHARE_TRANSLATIONS_BETWEEN_EXECUTORS=$<BOOL:${UML_JIT_SHARE_TRANSLATIONS_BETWEEN_EXECUTORS}>
    UML_JIT_CARRY_X8_X9_ACROSS_LINKS=$<BOOL:${UML_JIT_CARRY_X8_X9_ACROSS_LINKS}>
    $<$<BOOL:${DARWIN_UML_JIT_GPR_LINK_PROFILE}>:DARWIN_UML_JIT_GPR_LINK_PROFILE=1>
    $<$<BOOL:${DARWIN_UML_JIT_GPR_PRESSURE_PROFILE}>:DARWIN_UML_JIT_GPR_PRESSURE_PROFILE=1>)

if(UML_GUEST_16K_PAGES)
    set(UML_GUEST_PAGE_SIZE 16384)
    set(UML_GUEST_PAGE_SHIFT 14)
else()
    set(UML_GUEST_PAGE_SIZE 4096)
    set(UML_GUEST_PAGE_SHIFT 12)
endif()
if(UML_APPLE_PLATFORM MATCHES "^ios")
    # Both iOS products deliberately use the physical-device contract. The
    # signed ET_DYN kernel is linked at 48 GiB and biased at launch so its
    # native window ends at the measured 62 GiB device ceiling. The window is
    # exactly guest RAM plus a page-rounded one-eighth kernel-auxiliary
    # allowance, with Linux UML's 32 MiB minimum retained for small guests;
    # guest userspace is a sparse software-MMU range. Keep the existing
    # three-level 16 KiB guest page tables and use their maximum 47-bit
    # range; a true 48-bit range would add a page-table level to every iOS
    # kernel.
    set(UML_GUEST_ADDRESS_LIMIT 0x0000800000000000ULL)
    set(UML_NATIVE_ADDRESS_START 0x0000000c00000000ULL)
    set(UML_NATIVE_ADDRESS_END 0x0000000f80000000ULL)
    set(UML_NATIVE_LINK_ADDRESS 0x0000000c00000000ULL)
    set(UML_NATIVE_ADDRESS_CEILING 0x0000000f80000000ULL)
    set(UML_NATIVE_AUXILIARY_DIVISOR 8)
    set(UML_NATIVE_AUXILIARY_MINIMUM 0x0000000002000000ULL)
    set(UML_SIGRETURN_TRAMPOLINE 0x00007fffffffff00ULL)
    set(UML_DYNAMIC_NATIVE_LAYOUT 1)
    set(UML_SIGNED_KERNEL_CARRIER 1)
else()
    set(UML_GUEST_ADDRESS_LIMIT 0x0000600000000000ULL)
    set(UML_NATIVE_ADDRESS_START 0x0000600000000000ULL)
    set(UML_NATIVE_ADDRESS_END 0x0000600000000000ULL)
    set(UML_NATIVE_LINK_ADDRESS 0x0000600000000000ULL)
    set(UML_NATIVE_ADDRESS_CEILING 0x0000600000000000ULL)
    set(UML_NATIVE_AUXILIARY_DIVISOR 1)
    set(UML_NATIVE_AUXILIARY_MINIMUM 0ULL)
    set(UML_SIGRETURN_TRAMPOLINE 0x00003fffffffff00ULL)
    set(UML_DYNAMIC_NATIVE_LAYOUT 0)
    set(UML_SIGNED_KERNEL_CARRIER 0)
endif()
add_library(uml_guest_page_configuration INTERFACE)
add_library(UML::GuestPageConfiguration ALIAS
    uml_guest_page_configuration)
target_compile_definitions(uml_guest_page_configuration INTERFACE
    DARWIN_UML_GUEST_PAGE_SIZE=${UML_GUEST_PAGE_SIZE}
    DARWIN_UML_GUEST_PAGE_SHIFT=${UML_GUEST_PAGE_SHIFT}
    DARWIN_UML_GUEST_ADDRESS_LIMIT=${UML_GUEST_ADDRESS_LIMIT}
    DARWIN_UML_NATIVE_ADDRESS_START=${UML_NATIVE_ADDRESS_START}
    DARWIN_UML_NATIVE_ADDRESS_END=${UML_NATIVE_ADDRESS_END}
    DARWIN_UML_NATIVE_LINK_ADDRESS=${UML_NATIVE_LINK_ADDRESS}
    DARWIN_UML_NATIVE_ADDRESS_CEILING=${UML_NATIVE_ADDRESS_CEILING}
    DARWIN_UML_NATIVE_AUXILIARY_DIVISOR=${UML_NATIVE_AUXILIARY_DIVISOR}
    DARWIN_UML_NATIVE_AUXILIARY_MINIMUM=${UML_NATIVE_AUXILIARY_MINIMUM}
    DARWIN_UML_SIGRETURN_TRAMPOLINE=${UML_SIGRETURN_TRAMPOLINE}
    DARWIN_UML_DYNAMIC_NATIVE_LAYOUT=${UML_DYNAMIC_NATIVE_LAYOUT}
    DARWIN_UML_SIGNED_KERNEL_CARRIER=${UML_SIGNED_KERNEL_CARRIER})

if(UML_BUILD_UNIT_TESTS OR UML_BUILD_INTEGRATION_TESTS OR
   UML_BUILD_TOOLING_TESTS)
    enable_testing()
endif()

if(UML_TARGET_MACOS AND (UML_BUILD_GPU OR UML_BUILD_NETWORK_PROVIDER))
    add_subdirectory(dependencies)
endif()

if((UML_BUILD_GPU OR UML_BUILD_NETWORK_PROVIDER) AND NOT UML_TARGET_MACOS)
    if(NOT UML_APPLE_PLATFORM MATCHES "^ios")
        message(FATAL_ERROR
            "The Kernarium GPU and network providers require an iOS target")
    endif()
    if(UML_BUILD_GPU)
        include(Kernarium/Runtime/cmake/IosGpuDependencies.cmake)
    endif()
    if(UML_BUILD_NETWORK_PROVIDER)
        include(Kernarium/Runtime/cmake/IosNetworkDependencies.cmake)
    endif()
endif()

if(UML_BUILD_CLI)
    add_subdirectory(kernel)
endif()

add_subdirectory(darwin)

if(UML_BUILD_KERNARIUM_RUNTIME)
    add_subdirectory(Kernarium/Runtime)
endif()

if(UML_BUILD_UNIT_TESTS OR UML_BUILD_INTEGRATION_TESTS OR
   UML_BUILD_TOOLING_TESTS)
    add_subdirectory(tests)
endif()

uml_write_architecture_manifest()

# This structural gate is part of every Apple product configuration, including
# iOS builds with behavioral tests disabled. The File API reply is produced by
# the completed configure and the target never reconfigures its own tree.
get_property(_uml_audit_test_suites GLOBAL PROPERTY
    UML_AUDIT_EXPECTED_TEST_SUITES)
get_property(_uml_audit_conditional_tests GLOBAL PROPERTY
    UML_AUDIT_REQUIRED_CONDITIONAL_TESTS)
if(_uml_audit_test_suites)
    add_custom_target(architecture-audit
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
            -DSOURCE_SIZE_REPORT=${CMAKE_BINARY_DIR}/reports/source-sizes.tsv
            -P ${PROJECT_SOURCE_DIR}/scripts/audit-source-tree.cmake
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_ROOT=${CMAKE_BINARY_DIR}
            -P ${PROJECT_SOURCE_DIR}/scripts/audit-cmake-file-api.cmake
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_ROOT=${CMAKE_BINARY_DIR}
            -DMANIFEST=${PROJECT_SOURCE_DIR}/tests/test-manifest.tsv
            -DEXPECTED_SUITES=${_uml_audit_test_suites}
            -DREQUIRED_CONDITIONAL_TESTS=${_uml_audit_conditional_tests}
            -P ${PROJECT_SOURCE_DIR}/scripts/audit-test-manifest.cmake
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Auditing host source-tree architecture"
        VERBATIM)
else()
    add_custom_target(architecture-audit
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
            -DSOURCE_SIZE_REPORT=${CMAKE_BINARY_DIR}/reports/source-sizes.tsv
            -P ${PROJECT_SOURCE_DIR}/scripts/audit-source-tree.cmake
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_ROOT=${CMAKE_BINARY_DIR}
            -P ${PROJECT_SOURCE_DIR}/scripts/audit-cmake-file-api.cmake
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Auditing host source-tree architecture"
        VERBATIM)
endif()
