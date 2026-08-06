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
else()
    set(UML_TARGET_MACOS OFF)
    option(UML_BUILD_CLI "Build the arm64 macOS command-line app" OFF)
    set(UML_BUILD_KERNEL_RUNNER OFF CACHE BOOL
        "The transitional ELF runner is available only on macOS" FORCE)
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
    "Immutable lower rootfs image used by integration tests")
set(UML_INSTRUCTION_MATERIALIZATION_WORK_ROOT "" CACHE PATH
    "Optional external root for large instruction-materialization join state")
option(UML_ENABLE_EXTERNAL_NETWORK_TESTS
    "Register guest tests that intentionally access the public network" OFF)
set(UML_EXTERNAL_NETWORK_TEST_HOST "example.com" CACHE STRING
    "Public host used only when UML_ENABLE_EXTERNAL_NETWORK_TESTS is enabled")
set(UML_DEPENDENCY_PREFIX
    "${PROJECT_SOURCE_DIR}/.build/dependencies/macos-arm64"
    CACHE PATH "Private prefix containing source-built host dependencies")
option(UML_BUILD_GPU "Build the host GPU/display provider" ${UML_TARGET_MACOS})
option(UML_BUILD_NETWORK_PROVIDER
    "Build the optional SLiRP network provider"
    ${UML_BUILD_KERNEL_RUNNER})

option(UML_GUEST_16K_PAGES
    "Build the kernel and static userspace MMU for 16 KiB guest pages" ON)
option(DARWIN_UML_JIT_GPR_LINK_PROFILE
    "Instrument successful JIT links for guest-GPR traffic profiling" OFF)
option(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE
    "Instrument JIT translation for resident-GPR pressure profiling" OFF)
option(UML_ENABLE_RUNTIME_PROFILING
    "Compile optional runtime, MMU, and device profiling into products" OFF)

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
    $<$<BOOL:${DARWIN_UML_JIT_GPR_LINK_PROFILE}>:DARWIN_UML_JIT_GPR_LINK_PROFILE=1>
    $<$<BOOL:${DARWIN_UML_JIT_GPR_PRESSURE_PROFILE}>:DARWIN_UML_JIT_GPR_PRESSURE_PROFILE=1>)

if(UML_GUEST_16K_PAGES)
    set(UML_GUEST_PAGE_SIZE 16384)
    set(UML_GUEST_PAGE_SHIFT 14)
else()
    set(UML_GUEST_PAGE_SIZE 4096)
    set(UML_GUEST_PAGE_SHIFT 12)
endif()
add_library(uml_guest_page_configuration INTERFACE)
add_library(UML::GuestPageConfiguration ALIAS
    uml_guest_page_configuration)
target_compile_definitions(uml_guest_page_configuration INTERFACE
    DARWIN_UML_GUEST_PAGE_SIZE=${UML_GUEST_PAGE_SIZE}
    DARWIN_UML_GUEST_PAGE_SHIFT=${UML_GUEST_PAGE_SHIFT})

if(UML_BUILD_UNIT_TESTS OR UML_BUILD_INTEGRATION_TESTS OR
   UML_BUILD_TOOLING_TESTS)
    enable_testing()
endif()

if(UML_TARGET_MACOS AND (UML_BUILD_GPU OR UML_BUILD_NETWORK_PROVIDER))
    add_subdirectory(dependencies)
endif()

if(UML_BUILD_CLI)
    add_subdirectory(kernel)
endif()

add_subdirectory(darwin)

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
