# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_api_contract INTERFACE)
add_library(UML::ApiContract ALIAS uml_api_contract)
uml_declare_architecture_target(uml_api_contract api)
target_link_libraries(uml_api_contract INTERFACE
    UML::JitProfileConfiguration)
target_include_directories(uml_api_contract INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(uml_api_contract INTERFACE cxx_std_20)
