# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host-side composition only.  Each architectural layer owns its source
# manifest and targets in its own directory; this file states their order.

add_subdirectory(api)
add_subdirectory(platform)
add_subdirectory(memory)
add_subdirectory(cpu)
add_subdirectory(device/network)
add_subdirectory(device/input/vnc)

if(UML_TARGET_MACOS AND UML_BUILD_GPU)
    add_subdirectory(device/gpu)
endif()

add_subdirectory(runtime)
add_subdirectory(emulator)

if(UML_BUILD_CLI)
    if(NOT UML_TARGET_MACOS)
        message(FATAL_ERROR "The CLI is currently supported only for macOS")
    endif()
    if(NOT UML_BUILD_GPU)
        message(FATAL_ERROR "UML_BUILD_CLI requires UML_BUILD_GPU=ON")
    endif()
    add_subdirectory(app/macos)
endif()
