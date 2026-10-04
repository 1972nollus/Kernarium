# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_x86_64_gadget_bootstrap_tests
    x86_64_gadget_bootstrap_tests.cpp)
target_link_libraries(uml_x86_64_gadget_bootstrap_tests PRIVATE
    uml_x86_64_target
    uml_x86_64_gadgets
    UML::CpuContract)
target_include_directories(uml_x86_64_gadget_bootstrap_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME x86-64-gadget-bootstrap
    COMMAND uml_x86_64_gadget_bootstrap_tests
    LABELS cpu;x86-64)

add_executable(uml_x86_64_sse_exactness_tests
    x86_64_sse_exactness_tests.cpp)
target_link_libraries(uml_x86_64_sse_exactness_tests PRIVATE
    uml_x86_64_target
    uml_x86_64_gadgets
    UML::CpuContract)
target_include_directories(uml_x86_64_sse_exactness_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME x86-64-sse-exactness
    COMMAND uml_x86_64_sse_exactness_tests
    LABELS cpu;x86-64)

add_executable(uml_x86_64_code_page_registry_tests
    code_page_registry_tests.cpp)
target_link_libraries(uml_x86_64_code_page_registry_tests PRIVATE
    uml_x86_64_target
    UML::CpuContract)
target_include_directories(uml_x86_64_code_page_registry_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME x86-64-code-page-registry
    COMMAND uml_x86_64_code_page_registry_tests
    LABELS cpu;x86-64)

# K instruction traces and complementary FEX assembly programs are normalized
# into this checked-in pack. The ordinary unit test therefore has no runtime or
# build dependency on either upstream project or on an x86 assembler.
set(_uml_x86_64_instruction_corpus
    "${CMAKE_CURRENT_SOURCE_DIR}/corpus/x86_64_v3.pack")
if(NOT EXISTS "${_uml_x86_64_instruction_corpus}")
    message(FATAL_ERROR
        "The checked-in x86-64 instruction corpus is missing: "
        "${_uml_x86_64_instruction_corpus}")
endif()
add_executable(uml_x86_64_fex_asm_runner
    fex_asm_runner.cpp)
target_link_libraries(uml_x86_64_fex_asm_runner PRIVATE
    uml_x86_64_target
    uml_x86_64_gadgets
    UML::CpuContract)
target_include_directories(uml_x86_64_fex_asm_runner PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
set_target_properties(uml_x86_64_fex_asm_runner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
uml_add_unit_test(
    NAME x86-64-instruction-corpus
    COMMAND uml_x86_64_fex_asm_runner "${_uml_x86_64_instruction_corpus}"
    LABELS cpu;x86-64)
set_tests_properties(x86-64-instruction-corpus PROPERTIES TIMEOUT 900)
