cmake_minimum_required(VERSION 3.25)

project(uml_apple LANGUAGES CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(UML_APPLE_PLATFORM "macos" CACHE STRING
    "Apple target platform: macos, ios, or ios-simulator")
set_property(CACHE UML_APPLE_PLATFORM PROPERTY STRINGS macos ios ios-simulator)

if(NOT UML_APPLE_PLATFORM MATCHES "^(macos|ios|ios-simulator)$")
    message(FATAL_ERROR "Unsupported UML_APPLE_PLATFORM=${UML_APPLE_PLATFORM}")
endif()

if(NOT APPLE)
    message(FATAL_ERROR "The UML Apple backend requires an Apple SDK")
endif()

if(UML_APPLE_PLATFORM STREQUAL "macos")
    set(UML_TARGET_MACOS ON)
    option(UML_BUILD_CLI "Build the arm64 macOS command-line frontend" ON)
    option(UML_BUILD_KERNEL_RUNNER
           "Build the embedded ELF kernel runner for the macOS CLI" ON)
else()
    set(UML_TARGET_MACOS OFF)
    option(UML_BUILD_CLI "Build the arm64 macOS command-line frontend" OFF)
    set(UML_BUILD_KERNEL_RUNNER OFF CACHE BOOL
        "The transitional ELF runner is available only on macOS" FORCE)
endif()

option(UML_BUILD_TESTS "Build host-runnable backend unit tests" ${UML_TARGET_MACOS})
option(UML_ENABLE_GADGET_INTERPRETER_RUNPATH
    "Route non-control gadget instructions through the interpreter for debugging" OFF)
if(UML_ENABLE_GADGET_INTERPRETER_RUNPATH AND
   NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
        "UML_ENABLE_GADGET_INTERPRETER_RUNPATH is debug-only; configure with CMAKE_BUILD_TYPE=Debug")
endif()

if(UML_BUILD_TESTS)
    enable_testing()
    add_test(NAME source-tree-cxx-only
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
            -P ${PROJECT_SOURCE_DIR}/scripts/audit-source-tree.cmake)
endif()

add_subdirectory(darwin/backend)

if(UML_BUILD_CLI)
    if(NOT UML_TARGET_MACOS)
        message(FATAL_ERROR "The CLI is currently supported only for macOS")
    endif()
    add_subdirectory(darwin/frontend/macos)
endif()
