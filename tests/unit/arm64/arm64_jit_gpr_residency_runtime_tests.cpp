// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;

namespace {

constexpr std::uint32_t system_call = 0xd400'0001U;

[[nodiscard]] constexpr std::uint32_t
add_register(unsigned destination, unsigned left,
             unsigned right) noexcept {
  return 0x8b00'0000U | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

constexpr std::array instructions{
    add_register(0U, 1U, 1U),
    add_register(2U, 3U, 3U),
    add_register(4U, 5U, 5U),
    add_register(6U, 7U, 7U),
    add_register(8U, 9U, 9U),
    add_register(8U, 8U, 9U),
    add_register(8U, 8U, 9U),
    add_register(8U, 8U, 9U),
};

void repeated_late_pair_matches_the_specification(
    engine::EngineHarness &harness) {
  std::array<engine::ProgramWord, instructions.size() + 1U> code{};
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    code[index] = {
        .offset = index * sizeof(std::uint32_t),
        .instruction = instructions[index],
    };
  }
  code.back() = {
      .offset = instructions.size() * sizeof(std::uint32_t),
      .instruction = system_call,
  };

  arm64::CpuState initial{};
  for (unsigned reg = 0; reg < 31U; ++reg)
    initial.registers[reg] =
        0x1020'3040'5060'7080ULL +
        static_cast<std::uint64_t>(reg) * 0x0101'0101'0101'0101ULL;
  initial.pc = engine::engine_test_code_base;

  auto expected = initial;
  expected.registers[0] = expected.registers[1] * 2U;
  expected.registers[2] = expected.registers[3] * 2U;
  expected.registers[4] = expected.registers[5] * 2U;
  expected.registers[6] = expected.registers[7] * 2U;
  expected.registers[8] = expected.registers[9] * 2U;
  expected.registers[8] += expected.registers[9];
  expected.registers[8] += expected.registers[9];
  expected.registers[8] += expected.registers[9];
  expected.pc = engine::engine_test_code_base +
                code.size() * sizeof(std::uint32_t);

  harness.check_program({
      .name = "jit-gpr-residency-repeated-late-pair",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = engine::NativeRequirements::baseline(),
      .expected_instructions = code.size(),
  });
}

} // namespace

int main() {
  engine::EngineHarness harness;
  repeated_late_pair_matches_the_specification(harness);
  harness.print_summary("arm64-jit-gpr-residency-runtime");
  return 0;
}
