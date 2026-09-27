# SPDX-License-Identifier: GPL-2.0-or-later

if(UML_BUILD_NETWORK_PROVIDER)
    add_library(uml_network_provider_test_variant STATIC
        ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp/provider.cpp
        ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp/event_loop.cpp
        ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp/frame_queue.cpp)
    target_include_directories(uml_network_provider_test_variant
        PUBLIC
            ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp
            ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp/include
            ${PROJECT_SOURCE_DIR}/darwin/include
        PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin/device/network)
    target_compile_definitions(uml_network_provider_test_variant PUBLIC
        UML_SLIRP_TESTING=1)
    target_compile_features(uml_network_provider_test_variant PUBLIC cxx_std_20)
    target_compile_options(uml_network_provider_test_variant PRIVATE
        -Wall -Wextra -Werror)
    target_link_libraries(uml_network_provider_test_variant PRIVATE
        UML::GuestTimerMask
        UML::ThirdPartySlirp)
    add_dependencies(uml_network_provider_test_variant
        uml_dependency_prefix_verify)

    add_executable(uml_slirp_network_tests
        ${PROJECT_SOURCE_DIR}/tests/tooling/network/slirp_arp_probe.cpp)
    target_link_libraries(uml_slirp_network_tests PRIVATE
        uml_network_provider_test_variant
        UML::ThirdPartySlirp)
    target_include_directories(uml_slirp_network_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp)
    target_compile_features(uml_slirp_network_tests PRIVATE cxx_std_20)
    target_compile_options(uml_slirp_network_tests PRIVATE
        -Wall -Wextra -Werror)

    uml_add_tooling_test(
        NAME slirp-network-arp
        COMMAND uml_slirp_network_tests
        LABELS network
        TIMEOUT 30)

    add_executable(uml_slirp_forward_tests
        ${PROJECT_SOURCE_DIR}/tests/tooling/network/slirp_forward_probe.cpp)
    target_link_libraries(uml_slirp_forward_tests PRIVATE
        uml_network_provider_test_variant
        UML::ThirdPartySlirp)
    target_include_directories(uml_slirp_forward_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp)
    target_compile_features(uml_slirp_forward_tests PRIVATE cxx_std_20)
    target_compile_options(uml_slirp_forward_tests PRIVATE
        -Wall -Wextra -Werror)

    uml_add_tooling_test(
        NAME slirp-network-forward-specification
        COMMAND uml_slirp_forward_tests
        LABELS network
        TIMEOUT 30)

    add_executable(uml_slirp_transmit_backpressure_tests
        ${PROJECT_SOURCE_DIR}/tests/tooling/network/slirp_transmit_backpressure_probe.cpp)
    target_link_libraries(uml_slirp_transmit_backpressure_tests PRIVATE
        uml_network_provider_test_variant
        UML::ThirdPartySlirp)
    target_include_directories(uml_slirp_transmit_backpressure_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp)
    target_compile_features(uml_slirp_transmit_backpressure_tests PRIVATE
        cxx_std_20)
    target_compile_options(uml_slirp_transmit_backpressure_tests PRIVATE
        -Wall -Wextra -Werror)

    uml_add_tooling_test(
        NAME slirp-network-transmit-backpressure
        COMMAND uml_slirp_transmit_backpressure_tests
        LABELS network
        TIMEOUT 30)
endif()
