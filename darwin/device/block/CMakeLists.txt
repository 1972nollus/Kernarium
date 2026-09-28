# SPDX-License-Identifier: GPL-2.0-or-later

# The rule for naming a host file in a UBD kernel argument, shared by every
# product that composes one.
add_library(uml_device_ubd_path INTERFACE)
add_library(UML::DeviceUbdPath ALIAS uml_device_ubd_path)
uml_declare_architecture_target(uml_device_ubd_path device)
target_include_directories(uml_device_ubd_path INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_device_ubd_path INTERFACE cxx_std_20)

add_library(uml_android_boot STATIC android_boot.cpp)
add_library(UML::AndroidBoot ALIAS uml_android_boot)
uml_declare_architecture_target(uml_android_boot device)
target_include_directories(uml_android_boot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_android_boot PUBLIC cxx_std_20)
target_link_libraries(uml_android_boot PUBLIC UML::DeviceUbdPath)
target_compile_options(uml_android_boot PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_android_boot PROPERTIES POSITION_INDEPENDENT_CODE ON)
