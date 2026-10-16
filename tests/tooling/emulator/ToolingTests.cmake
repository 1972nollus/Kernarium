# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME guest-harness-attached-session
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/emulator/guest_harness_session_tests.py
    LABELS emulator;guest
    TIMEOUT 180)

if(UML_BUILD_KERNEL_RUNNER)
    add_executable(uml_embedded_kernel_serialization_tests
        ${PROJECT_SOURCE_DIR}/tests/tooling/emulator/embedded_kernel_serialization_tests.cpp)
    target_link_libraries(uml_embedded_kernel_serialization_tests PRIVATE
        UML::BridgeAbi
        uml_emulator_test_variant
        UML::GuestPageConfiguration)

    uml_add_tooling_test(
        NAME embedded-kernel-handle-serialization
        COMMAND uml_embedded_kernel_serialization_tests
        LABELS emulator;loader
        TIMEOUT 30)
endif()
