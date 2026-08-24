# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_android_boot STATIC android_boot.cpp)
add_library(UML::AndroidBoot ALIAS uml_android_boot)
uml_declare_architecture_target(uml_android_boot device)
target_include_directories(uml_android_boot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_android_boot PUBLIC cxx_std_20)
target_compile_options(uml_android_boot PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_android_boot PROPERTIES POSITION_INDEPENDENT_CODE ON)
