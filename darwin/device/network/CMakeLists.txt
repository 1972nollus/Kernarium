# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_network_device OBJECT device.cpp)
add_library(UML::NetworkDevice ALIAS uml_network_device)
uml_declare_architecture_target(uml_network_device device)
target_include_directories(uml_network_device PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_network_device PUBLIC UML::BridgeAbi)
target_link_libraries(uml_network_device PRIVATE UML::RunRouting)

if(UML_BUILD_NETWORK_PROVIDER)
    add_library(uml_network_provider STATIC
        slirp/provider.cpp
        slirp/event_loop.cpp
        slirp/frame_queue.cpp)
    add_library(UML::NetworkProvider ALIAS uml_network_provider)
    uml_declare_architecture_target(uml_network_provider device)
    target_include_directories(uml_network_provider
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/slirp/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/slirp)
    target_compile_definitions(uml_network_provider PRIVATE
        UML_SLIRP_TESTING=0)

    target_link_libraries(uml_network_provider PUBLIC UML::BridgeAbi)
    target_link_libraries(uml_network_provider PRIVATE
        UML::GuestTimerMask
        UML::ThirdPartySlirp)
    if(TARGET uml_dependency_prefix_verify)
        add_dependencies(uml_network_provider uml_dependency_prefix_verify)
    endif()
    if(TARGET uml_ios_network_prefix_verify)
        add_dependencies(uml_network_provider
            uml_ios_network_prefix_verify)
    endif()

    if(UML_BUILD_TOOLING_TESTS)
        # The provider with its test hooks, for the SLiRP probes; products
        # build it with UML_SLIRP_TESTING=0.
        add_library(uml_network_provider_test_variant STATIC
            slirp/provider.cpp
            slirp/event_loop.cpp
            slirp/frame_queue.cpp)
        uml_declare_architecture_target(uml_network_provider_test_variant
            device)
        target_include_directories(uml_network_provider_test_variant
            PUBLIC
                ${CMAKE_CURRENT_SOURCE_DIR}/slirp
                ${CMAKE_CURRENT_SOURCE_DIR}/slirp/include
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(uml_network_provider_test_variant PUBLIC
            UML_SLIRP_TESTING=1)
        target_link_libraries(uml_network_provider_test_variant PUBLIC
            UML::BridgeAbi)
        target_link_libraries(uml_network_provider_test_variant PRIVATE
            UML::GuestTimerMask
            UML::ThirdPartySlirp)
        if(TARGET uml_dependency_prefix_verify)
            add_dependencies(uml_network_provider_test_variant
                uml_dependency_prefix_verify)
        endif()
    endif()
endif()
