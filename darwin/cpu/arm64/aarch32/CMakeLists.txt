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
