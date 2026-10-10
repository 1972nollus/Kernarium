# SPDX-License-Identifier: GPL-2.0-or-later

# The AArch32 decoder: one generated encoding table and the code that reads it
# (README.md). It is engine-neutral and holds no state,
# so the gadget engine, the JIT and the semantic library can all link it; it
# includes no engine.
add_library(uml_arm64_aarch32_decoder OBJECT
    a32_encodings.cpp
    a32_operand_decode.cpp
    decode.cpp)
uml_declare_architecture_target(uml_arm64_aarch32_decoder cpu shared)
target_include_directories(uml_arm64_aarch32_decoder PUBLIC
    ${PROJECT_SOURCE_DIR}/darwin)

# The AArch32 semantic library (semantics.hpp): the engines' slow paths and
# the corpus's model lane execute any instruction of the profile through it.
# It works on CpuState alone and includes no engine, so it is a shared CPU
# target. Nothing links it into a product until an engine calls it.
add_library(uml_arm64_aarch32_semantics OBJECT
    semantics.cpp
    semantics_control.cpp
    semantics_integer.cpp
    semantics_memory.cpp
    semantics_neon_integer.cpp
    semantics_table.cpp
    semantics_vector_memory.cpp
    semantics_vector_move.cpp)
uml_declare_architecture_target(uml_arm64_aarch32_semantics cpu shared)
target_link_libraries(uml_arm64_aarch32_semantics PRIVATE
    UML::BridgeAbi
    UML::CpuContract)
target_include_directories(uml_arm64_aarch32_semantics PUBLIC
    ${PROJECT_SOURCE_DIR}/darwin)
