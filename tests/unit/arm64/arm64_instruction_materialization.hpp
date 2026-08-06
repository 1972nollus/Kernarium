// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_instruction_evidence.hpp"

#include <cstdint>
#include <source_location>
#include <span>
#include <string_view>

namespace uml::emulator::tests {

struct InstructionInputMaterialization final {
  std::string_view case_name;
  std::span<const ProgramWord> code;
  std::size_t entry_offset{};
  std::span<const std::uint32_t> evidence_words;
  std::span<const std::byte> installed_code_page;
  const arm64::CpuState &initial;
  const arm64::CpuState &expected;
  std::span<const MemoryRegion> memory;
  std::string_view expected_event;
  std::string_view expected_retirement;
  std::size_t instruction_budget{};
  std::string_view expected_state_oracle;
  std::string_view expected_fault;
  std::uint32_t expected_syndrome{};
  NativeRequirements native;
  InstructionNativeScope native_scope{InstructionNativeScope::instruction};
  bool invalid_encoding{};
  bool has_custom_validator{};
  bool has_before_engine_hook{};
  bool has_after_engine_hook{};
  std::string_view validator_relation_id;
  std::string_view before_engine_effect_id;
  std::string_view after_engine_effect_id;
  std::uint64_t initial_state_digest{};
  std::uint64_t initial_memory_digest{};
  std::uint64_t expected_state_digest{};
  std::uint64_t expected_memory_digest{};
  InstructionEngineEvidence engines;
  InstructionNativeEvidence native_observation;
};

/*
 * Raw input materialization is evidence-only. Combined unit+tooling graphs
 * assign every producer a unique UML_INSTRUCTION_INPUT_MATERIALIZATION path.
 * Ordinary unit-only graphs and every production target leave it unset,
 * making this a single predictable branch with no serialization or retained
 * state. UML_INSTRUCTION_RAW_EVIDENCE remains a manual compatibility switch
 * that derives the historical sidecar path from UML_INSTRUCTION_EVIDENCE.
 */
void record_instruction_input_materialization(
    const InstructionInputMaterialization &materialization,
    const std::source_location &location);

} // namespace uml::emulator::tests
