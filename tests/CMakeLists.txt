# SPDX-License-Identifier: GPL-2.0-or-later

if(UML_BUILD_UNIT_TESTS)
    add_library(uml_host_capabilities_test_support OBJECT
        unit/host_capabilities_test_support.cpp)
    target_link_libraries(uml_host_capabilities_test_support PRIVATE
        UML::CpuContract
        UML::PlatformContract)
    target_include_directories(uml_host_capabilities_test_support PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_features(uml_host_capabilities_test_support PRIVATE
        cxx_std_20)
    target_compile_options(uml_host_capabilities_test_support PRIVATE
        -Wall -Wextra -Werror)
    # Unit binaries are independent consumers of the same scoped build
    # configuration as production targets; no directory-global definitions
    # are needed.
    link_libraries(
        uml_host_capabilities_test_support
        UML::GuestPageConfiguration
        UML::JitProfileConfiguration)
    add_subdirectory(unit)
endif()

if(UML_BUILD_TOOLING_TESTS)
    add_subdirectory(tooling)
    add_custom_target(tooling-test-binaries)
    get_property(_uml_tooling_targets DIRECTORY
        "${CMAKE_CURRENT_SOURCE_DIR}/tooling" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_tooling_target IN LISTS _uml_tooling_targets)
        get_target_property(_tooling_type ${_tooling_target} TYPE)
        if(_tooling_type STREQUAL "EXECUTABLE")
            add_dependencies(tooling-test-binaries ${_tooling_target})
        endif()
    endforeach()
endif()

if(UML_BUILD_INTEGRATION_TESTS)
    # CTest commands reference these host fixtures directly. Keep their build
    # ownership explicit so the integration preset produces every executable
    # required by the registered suite, not only the emulator app.
    add_custom_target(integration-test-binaries)
    add_subdirectory(integration)
    foreach(_integration_target IN ITEMS uml-darwin uml-virtio-gpu)
        if(NOT TARGET ${_integration_target})
            message(FATAL_ERROR
                "Integration target is not available: ${_integration_target}")
        endif()
        add_dependencies(integration-test-binaries ${_integration_target})
    endforeach()
endif()

# Every CTest registration goes through an explicit suite helper.  Validate
# that it has a retained manifest entry before generation completes; this
# rejects accidental test additions, omissions, and suite drift immediately.
uml_validate_registered_test_manifest()

set(_uml_enabled_test_suites)
if(UML_BUILD_TOOLING_TESTS)
    list(APPEND _uml_enabled_test_suites tooling)
endif()
if(UML_BUILD_UNIT_TESTS)
    list(APPEND _uml_enabled_test_suites unit)
endif()
if(UML_BUILD_INTEGRATION_TESTS)
    list(APPEND _uml_enabled_test_suites integration)
endif()
string(JOIN "," _uml_enabled_test_suites_csv ${_uml_enabled_test_suites})

# The manifest keeps configuration-specific coverage visible without making a
# 4 KiB-only test appear missing from the 16 KiB registry. The audit receives
# the active conditional registrations explicitly, so it still rejects an
# accidental omission from the configuration where the test applies.
set(_uml_required_conditional_tests)
if(UML_BUILD_UNIT_TESTS AND NOT UML_GUEST_16K_PAGES)
    list(APPEND _uml_required_conditional_tests native-4k-shadow-mmu)
endif()
if(UML_BUILD_UNIT_TESTS AND UML_BUILD_TOOLING_TESTS)
    list(APPEND _uml_required_conditional_tests
        arm64-instruction-evidence)
endif()
if(UML_BUILD_UNIT_TESTS)
    if(DARWIN_UML_JIT_GPR_LINK_PROFILE)
        list(APPEND _uml_required_conditional_tests
            arm64-jit-gpr-link-profile-codegen
            arm64-jit-gpr-link-profile-runtime)
    endif()
    if(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
        list(APPEND _uml_required_conditional_tests
            arm64-jit-gpr-pressure-profile)
    endif()
endif()
if(TARGET uml_slirp_network_tests)
    list(APPEND _uml_required_conditional_tests slirp-network-arp)
endif()
if(TARGET uml_embedded_kernel_serialization_tests)
    list(APPEND _uml_required_conditional_tests
        embedded-kernel-handle-serialization)
endif()
if(TARGET uml-vnc-smoke-client)
    list(APPEND _uml_required_conditional_tests virtio-gpu-vnc-smoke)
endif()
if(UML_BUILD_INTEGRATION_TESTS AND UML_ENABLE_EXTERNAL_NETWORK_TESTS)
    list(APPEND _uml_required_conditional_tests
        guest-external-network
        guest-kde-vnc-webgl-aquarium
        guest-webgl-rendered-workflows)
endif()
string(JOIN "," _uml_required_conditional_tests_csv
    ${_uml_required_conditional_tests})
set_property(GLOBAL PROPERTY UML_AUDIT_EXPECTED_TEST_SUITES
    "${_uml_enabled_test_suites_csv}")
set_property(GLOBAL PROPERTY UML_AUDIT_REQUIRED_CONDITIONAL_TESTS
    "${_uml_required_conditional_tests_csv}")
