# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_device_guest_console STATIC guest_console.cpp)
add_library(UML::DeviceGuestConsole ALIAS uml_device_guest_console)
uml_declare_architecture_target(uml_device_guest_console device)
target_link_libraries(uml_device_guest_console PUBLIC UML::DeviceSocketPaths)
target_include_directories(uml_device_guest_console
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${PROJECT_SOURCE_DIR}/darwin/device)
target_compile_features(uml_device_guest_console PUBLIC cxx_std_20)
target_compile_options(uml_device_guest_console PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_device_guest_console PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
