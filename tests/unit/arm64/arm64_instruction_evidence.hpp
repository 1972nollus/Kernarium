// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/host_capabilities.hpp"

#include <cstdint>
#include <source_location>
#include <string_view>

namespace uml::emulator::tests {

enum class InstructionEngineResult {
  unavailable,
  skipped_host_feature,
  executed_not_retired,
  executed_retired,
};

enum class InstructionNativeScope {
  instruction,
  program,
};

struct InstructionEngineEvidence final {
  InstructionEngineResult interpreter{InstructionEngineResult::unavailable};
  InstructionEngineResult gadgets{InstructionEngineResult::unavailable};
  InstructionEngineResult jit{InstructionEngineResult::unavailable};
};

struct InstructionNativeEvidence final {
  ::uml::cpu::ArmFeatureSet required;
  ::uml::cpu::ArmFeatureSet observed;
};

struct InstructionExpectationEvidence final {
  std::string_view event;
  std::string_view retirement;
  std::string_view state_oracle;
  std::uint64_t initial_state_digest{};
  std::uint64_t initial_memory_digest{};
  std::uint64_t state_digest{};
  std::uint64_t memory_digest{};
  std::string_view fault;
  std::uint32_t syndrome{};
};

void record_instruction_evidence(
    std::string_view case_name, std::uint32_t instruction, bool invalid,
    InstructionEngineEvidence engines, InstructionNativeEvidence native,
    InstructionNativeScope native_scope,
    InstructionExpectationEvidence expectation,
    const std::source_location &location);

} // namespace uml::emulator::tests
