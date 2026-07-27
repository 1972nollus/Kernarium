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
option(UML_GUEST_16K_PAGES
    "Build the kernel and static userspace MMU for 16 KiB guest pages" ON)
option(UML_ENABLE_GADGET_INTERPRETER_RUNPATH
    "Route non-control gadget instructions through the interpreter for debugging" OFF)
if(UML_ENABLE_GADGET_INTERPRETER_RUNPATH AND
   NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
        "UML_ENABLE_GADGET_INTERPRETER_RUNPATH is debug-only; configure with CMAKE_BUILD_TYPE=Debug")
endif()

if(UML_GUEST_16K_PAGES)
    set(UML_GUEST_PAGE_SIZE 16384)
    set(UML_GUEST_PAGE_SHIFT 14)
else()
    set(UML_GUEST_PAGE_SIZE 4096)
    set(UML_GUEST_PAGE_SHIFT 12)
endif()
add_compile_definitions(
    DARWIN_UML_GUEST_PAGE_SIZE=${UML_GUEST_PAGE_SIZE}
    DARWIN_UML_GUEST_PAGE_SHIFT=${UML_GUEST_PAGE_SHIFT})

if(UML_BUILD_TESTS)
    enable_testing()
    add_test(NAME source-tree-cxx-only
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
            -P ${PROJECT_SOURCE_DIR}/scripts/audit-source-tree.cmake)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME gpu-guest-rootfs-safety
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/gpu/test_run_gpu_guest.py)
    set_tests_properties(gpu-guest-rootfs-safety PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tests/gpu)
    add_test(NAME kde-vnc-launcher-options
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/gpu/test_boot_kde_vnc.py)
endif()

add_subdirectory(darwin/backend)

if(UML_TARGET_MACOS)
    # First-party host code remains C++, but the vendored VNC implementation
    # is C and is built as part of the GPU service.
    enable_language(C)
    set(LIBVNCSERVER_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(WITH_LIBVNCSERVER ON CACHE BOOL "" FORCE)
    set(WITH_LIBVNCCLIENT ON CACHE BOOL "" FORCE)
    set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(WITH_ZLIB ON CACHE BOOL "" FORCE)
    set(WITH_LZO OFF CACHE BOOL "" FORCE)
    set(WITH_JPEG OFF CACHE BOOL "" FORCE)
    set(WITH_PNG OFF CACHE BOOL "" FORCE)
    set(WITH_SDL OFF CACHE BOOL "" FORCE)
    set(WITH_GTK OFF CACHE BOOL "" FORCE)
    set(WITH_LIBSSHTUNNEL OFF CACHE BOOL "" FORCE)
    set(WITH_GNUTLS OFF CACHE BOOL "" FORCE)
    set(WITH_OPENSSL OFF CACHE BOOL "" FORCE)
    set(WITH_SYSTEMD OFF CACHE BOOL "" FORCE)
    set(WITH_GCRYPT OFF CACHE BOOL "" FORCE)
    set(WITH_FFMPEG OFF CACHE BOOL "" FORCE)
    set(WITH_TIGHTVNC_FILETRANSFER OFF CACHE BOOL "" FORCE)
    set(WITH_WEBSOCKETS OFF CACHE BOOL "" FORCE)
    set(WITH_SASL OFF CACHE BOOL "" FORCE)
    set(WITH_XCB OFF CACHE BOOL "" FORCE)
    set(WITH_QT OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/libvncserver
        ${PROJECT_BINARY_DIR}/external/libvncserver EXCLUDE_FROM_ALL)
    add_subdirectory(darwin/gpu)
endif()

if(UML_BUILD_CLI)
    if(NOT UML_TARGET_MACOS)
        message(FATAL_ERROR "The CLI is currently supported only for macOS")
    endif()
    add_subdirectory(darwin/frontend/macos)
endif()
