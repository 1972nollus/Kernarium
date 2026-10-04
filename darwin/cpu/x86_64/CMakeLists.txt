# SPDX-License-Identifier: GPL-2.0-or-later

# What every x86-64 engine shares: the guest CPU state, the decoder, CPUID,
# the software TLB entry and the code-page registry. Its engine kind lets each
# engine link it without linking another engine.
add_library(uml_x86_64_target OBJECT
    cpu_state.cpp
    decoder.cpp
    code_page_registry.cpp)
uml_declare_architecture_target(uml_x86_64_target cpu shared)
target_link_libraries(uml_x86_64_target PRIVATE UML::CpuContract)
target_include_directories(uml_x86_64_target PUBLIC
    ${PROJECT_SOURCE_DIR}/darwin)

add_subdirectory(gadgets)
