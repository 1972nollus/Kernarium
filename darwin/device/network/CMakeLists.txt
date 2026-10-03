# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_network_device OBJECT device.cpp)
add_library(UML::NetworkDevice ALIAS uml_network_device)
uml_declare_architecture_target(uml_network_device device)
target_include_directories(uml_network_device PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_network_device PUBLIC UML::BridgeAbi)
target_link_libraries(uml_network_device PRIVATE UML::RunRouting)

# The product provider builds without its test hooks. The SLiRP probes drive
# a test variant that publishes the hooks and the provider's own headers.
function(_uml_add_network_provider TARGET TESTING)
    add_library(${TARGET} STATIC
        slirp/provider.cpp
        slirp/event_loop.cpp
        slirp/frame_queue.cpp)
    uml_declare_architecture_target(${TARGET} device)
    if(TESTING)
        target_include_directories(${TARGET} PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/slirp)
        target_compile_definitions(${TARGET} PUBLIC UML_SLIRP_TESTING=1)
    else()
        target_compile_definitions(${TARGET} PRIVATE UML_SLIRP_TESTING=0)
    endif()
    target_include_directories(${TARGET}
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/slirp/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/slirp)
    target_link_libraries(${TARGET} PUBLIC UML::BridgeAbi UML::Descriptor)
    target_link_libraries(${TARGET} PRIVATE
        UML::GuestTimerMask
        UML::ThirdPartySlirp)
    foreach(_verify IN ITEMS
            uml_dependency_prefix_verify uml_ios_network_prefix_verify)
        if(TARGET ${_verify})
            add_dependencies(${TARGET} ${_verify})
        endif()
    endforeach()
endfunction()

if(UML_BUILD_NETWORK_PROVIDER)
    _uml_add_network_provider(uml_network_provider 0)
    add_library(UML::NetworkProvider ALIAS uml_network_provider)
    if(UML_BUILD_TOOLING_TESTS)
        _uml_add_network_provider(uml_network_provider_test_variant 1)
    endif()
endif()
