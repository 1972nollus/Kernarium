# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME guest-harness-attached-session
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/emulator/guest_harness_session_tests.py
    LABELS emulator;guest
    TIMEOUT 120)

if(UML_BUILD_KERNEL_RUNNER)
    add_executable(uml_embedded_kernel_serialization_tests
        ${PROJECT_SOURCE_DIR}/tests/tooling/emulator/embedded_kernel_serialization_tests.cpp)
    target_link_libraries(uml_embedded_kernel_serialization_tests PRIVATE
        uml_emulator_admission_test_variant
        UML::GuestPageConfiguration)
    target_include_directories(uml_embedded_kernel_serialization_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${PROJECT_SOURCE_DIR}/darwin/emulator/src)
    target_compile_features(uml_embedded_kernel_serialization_tests PRIVATE
        cxx_std_20)
    target_compile_options(uml_embedded_kernel_serialization_tests PRIVATE
        -Wall -Wextra -Werror)
    target_compile_definitions(uml_embedded_kernel_serialization_tests PRIVATE
        UML_EMULATOR_ADMISSION_TESTING=1)

    uml_add_tooling_test(
        NAME embedded-kernel-handle-serialization
        COMMAND uml_embedded_kernel_serialization_tests
        LABELS emulator;loader
        TIMEOUT 30)
endif()
