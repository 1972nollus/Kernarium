// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <source_location>
#include <string_view>

namespace uml::emulator::tests {

enum class InstructionJitEvidence {
  executed,
  skipped_host_feature,
  unavailable,
};

struct InstructionEngineEvidence final {
  bool interpreter{};
  bool gadgets{};
  InstructionJitEvidence jit{InstructionJitEvidence::unavailable};
};

void record_instruction_evidence(
    std::string_view case_name, std::uint32_t instruction, bool invalid,
    InstructionEngineEvidence engines,
    const std::source_location &location);

} // namespace uml::emulator::tests
