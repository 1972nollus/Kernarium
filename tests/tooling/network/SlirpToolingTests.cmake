# SPDX-License-Identifier: GPL-2.0-or-later

if(UML_BUILD_NETWORK_PROVIDER)
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
