# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_platform_contract INTERFACE)
add_library(UML::PlatformContract ALIAS uml_platform_contract)
uml_declare_architecture_target(uml_platform_contract platform)
target_compile_features(uml_platform_contract INTERFACE cxx_std_20)
