# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_x86_64_gadget_bootstrap_tests
    x86_64_gadget_bootstrap_tests.cpp)
target_link_libraries(uml_x86_64_gadget_bootstrap_tests PRIVATE
    uml_x86_64_gadgets
    UML::CpuContract)
target_include_directories(uml_x86_64_gadget_bootstrap_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_x86_64_gadget_bootstrap_tests PRIVATE cxx_std_20)
target_compile_options(uml_x86_64_gadget_bootstrap_tests PRIVATE
    -Wall -Wextra -Werror)
uml_add_unit_test(
    NAME x86-64-gadget-bootstrap
    COMMAND uml_x86_64_gadget_bootstrap_tests
    LABELS cpu;x86-64)

# The complete FEX corpus is an opt-in external campaign.  Keep its native
# adapter out of the ordinary unit aggregate while making the exact gadgets
# execution path available as an explicit build target.
add_executable(uml_x86_64_fex_asm_runner EXCLUDE_FROM_ALL
    fex_asm_runner.cpp)
target_link_libraries(uml_x86_64_fex_asm_runner PRIVATE
    uml_x86_64_gadgets
    UML::CpuContract)
target_include_directories(uml_x86_64_fex_asm_runner PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_features(uml_x86_64_fex_asm_runner PRIVATE cxx_std_20)
target_compile_options(uml_x86_64_fex_asm_runner PRIVATE
    -Wall -Wextra -Werror)
set_target_properties(uml_x86_64_fex_asm_runner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
