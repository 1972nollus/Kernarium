# SPDX-License-Identifier: GPL-2.0-or-later

find_package(Python3 REQUIRED COMPONENTS Interpreter)

uml_add_tooling_test(
    NAME source-tree-cxx-only
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -DSOURCE_SIZE_REPORT=${CMAKE_BINARY_DIR}/reports/source-sizes.tsv
        -P ${PROJECT_SOURCE_DIR}/scripts/audit-source-tree.cmake
    LABELS audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME source-size-advisory-policy
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_SIZE_MODULE=${PROJECT_SOURCE_DIR}/cmake/SourceSizeReport.cmake
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/source-size-policy-fixture
        -P ${PROJECT_SOURCE_DIR}/scripts/test-source-size-report.cmake
    LABELS audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-release-source-preparer
    COMMAND ${PROJECT_SOURCE_DIR}/kernel/test-materialize-release.sh
    LABELS kernel;dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME abi-declaration-identity
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/abi/test_declaration_identity.sh
    LABELS abi;audit
    TIMEOUT 30)

# Source archive extraction and preparation intentionally exercise the
# Python-based build helper.  Keep them in the host tooling suite rather than
# presenting them as Python-free unit tests.
uml_add_tooling_test(
    NAME dependency-source-archive-verifier
    COMMAND ${PROJECT_SOURCE_DIR}/dependencies/test-source-archive.sh
    LABELS dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME dependency-source-preparer
    COMMAND ${PROJECT_SOURCE_DIR}/dependencies/test-prepare.sh
    LABELS dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME dependency-build-manifest
    COMMAND ${PROJECT_SOURCE_DIR}/dependencies/test-build-manifest.sh
    LABELS dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME dependency-prefix-verifier
    COMMAND ${PROJECT_SOURCE_DIR}/dependencies/test-verify-prefix.sh
    LABELS dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME ltp-runner-parser
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/ltp/run_guest_ltp_tests.py
    LABELS kernel;ltp
    TIMEOUT 120)

uml_add_tooling_test(
    NAME rootfs-provisioning
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tools/rootfs/test-provision.py
    LABELS rootfs;provisioning
    TIMEOUT 120)

uml_add_tooling_test(
    NAME rootfs-maintenance-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tools/rootfs/test-maintenance.py
    LABELS rootfs;maintenance
    TIMEOUT 120)

uml_add_tooling_test(
    NAME gpu-guest-rootfs-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_run_gpu_guest.py
    LABELS gpu
    TIMEOUT 120)

uml_add_tooling_test(
    NAME kde-vnc-launcher-options
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_boot_kde_vnc.py
    LABELS gpu;vnc
    TIMEOUT 120)

uml_add_tooling_test(
    NAME chromium-webgl-aquarium-metrics
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_run_chromium_webgl.py
    LABELS gpu;webgl
    TIMEOUT 120)

uml_add_tooling_test(
    NAME network-smoke-taxonomy
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/network/test_run_guest_network_smoke.py
    LABELS network;slirp
    TIMEOUT 120)

uml_add_tooling_test(
    NAME jit-gpr-link-profile-analysis
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/perf/analyze_jit_gpr_link_profile_tests.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tests/perf
    LABELS cpu;jit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-instruction-inventory
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/validate_a64_instruction_inventory.py
    LABELS cpu
    TIMEOUT 120)

if(UML_BUILD_UNIT_TESTS)
    uml_add_tooling_test(
        NAME arm64-instruction-evidence-reset
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_ROOT=${CMAKE_BINARY_DIR}
            -P ${PROJECT_SOURCE_DIR}/cmake/ResetInstructionEvidence.cmake
        FIXTURES_SETUP uml-instruction-evidence-run
        LABELS cpu;audit)

    get_property(_uml_unit_evidence_dependencies GLOBAL PROPERTY UML_UNIT_TESTS)
    uml_add_tooling_test(
        NAME arm64-instruction-evidence
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/validate_a64_instruction_evidence.py
            --evidence-directory ${CMAKE_BINARY_DIR}/instruction-evidence
            --build-root ${CMAKE_BINARY_DIR}
        LABELS cpu;audit
        TIMEOUT 120)
    # DEPENDS orders producers that are already selected. Deliberately do not
    # require the reset fixture here: selecting only this validator preserves
    # and checks an existing evidence directory instead of erasing it.
    set_tests_properties(arm64-instruction-evidence PROPERTIES
        DEPENDS "${_uml_unit_evidence_dependencies}")
endif()

if(UML_BUILD_NETWORK_PROVIDER)
    add_executable(uml_slirp_network_tests
        ${PROJECT_SOURCE_DIR}/tests/tooling/network/slirp_arp_probe.cpp)
    target_link_libraries(uml_slirp_network_tests PRIVATE
        UML::NetworkProvider)
    target_compile_features(uml_slirp_network_tests PRIVATE cxx_std_20)
    target_compile_options(uml_slirp_network_tests PRIVATE
        -Wall -Wextra -Werror)

    uml_add_tooling_test(
        NAME slirp-network-arp
        COMMAND uml_slirp_network_tests
        LABELS network
        TIMEOUT 30)
endif()

if(UML_BUILD_KERNEL_RUNNER)
    add_executable(uml_embedded_kernel_serialization_tests
        ${PROJECT_SOURCE_DIR}/tests/tooling/emulator/embedded_kernel_serialization_tests.cpp)
    target_link_libraries(uml_embedded_kernel_serialization_tests PRIVATE
        UML::Emulator)
    target_include_directories(uml_embedded_kernel_serialization_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_features(uml_embedded_kernel_serialization_tests PRIVATE
        cxx_std_20)
    target_compile_options(uml_embedded_kernel_serialization_tests PRIVATE
        -Wall -Wextra -Werror)

    uml_add_tooling_test(
        NAME embedded-kernel-handle-serialization
        COMMAND uml_embedded_kernel_serialization_tests
        LABELS emulator;loader
        TIMEOUT 30)
endif()

if(TARGET uml-virtio-gpu)
    add_executable(uml-vnc-smoke-client
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/vnc_smoke_client.cpp)
    target_link_libraries(uml-vnc-smoke-client PRIVATE
        UML::ThirdPartyVncClient
        "-framework CoreFoundation")
    target_compile_options(uml-vnc-smoke-client PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    set_target_properties(uml-vnc-smoke-client PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

    uml_add_tooling_test(
        NAME virtio-gpu-vnc-smoke
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/run_vnc_smoke.py
            --server $<TARGET_FILE:uml-virtio-gpu>
            --client $<TARGET_FILE:uml-vnc-smoke-client>
        LABELS gpu;vnc
        TIMEOUT 30
        PROCESSORS 1
        RESOURCE_GROUPS gpus:1)
endif()
