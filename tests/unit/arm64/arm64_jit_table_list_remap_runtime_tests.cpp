// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"
#include "arm64_simd_lut_test_support.hpp"
#include "arm64_simd_rearrange_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;
namespace lut = uml::emulator::tests::simd_lut;
namespace rearrange = arm64_simd_rearrange_test;

namespace {

constexpr std::uint32_t system_call = 0xd400'0001U;

[[nodiscard]] constexpr std::uint32_t
bitwise_xor(unsigned destination, unsigned left, unsigned right) noexcept {
  return 0x6e20'1c00U | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

void apply_bitwise_xor(arm64::CpuState &state, unsigned destination,
                       unsigned left, unsigned right) {
  const auto left_value = state.vectors[left].bytes;
  const auto right_value = state.vectors[right].bytes;
  for (unsigned byte = 0; byte < 16U; ++byte)
    state.vectors[destination].bytes[byte] =
        left_value[byte] ^ right_value[byte];
}

constexpr std::array interleaving{
    bitwise_xor(8U, 1U, 9U),
    bitwise_xor(10U, 2U, 11U),
    bitwise_xor(12U, 3U, 13U),
    bitwise_xor(14U, 4U, 15U),
};

void apply_interleaving(arm64::CpuState &state) {
  apply_bitwise_xor(state, 8U, 1U, 9U);
  apply_bitwise_xor(state, 10U, 2U, 11U);
  apply_bitwise_xor(state, 12U, 3U, 13U);
  apply_bitwise_xor(state, 14U, 4U, 15U);
}

template <std::size_t Size>
void check_program(engine::EngineHarness &harness, std::string_view name,
                   const std::array<std::uint32_t, Size> &instructions,
                   const arm64::CpuState &initial,
                   arm64::CpuState expected,
                   engine::NativeRequirements native) {
  std::array<engine::ProgramWord, Size + 1U> code{};
  for (std::size_t index = 0; index < Size; ++index) {
    code[index] = {
        .offset = index * sizeof(std::uint32_t),
        .instruction = instructions[index],
    };
  }
  code.back() = {
      .offset = Size * sizeof(std::uint32_t),
      .instruction = system_call,
  };
  expected.pc =
      engine::engine_test_code_base +
      code.size() * sizeof(std::uint32_t);
  harness.check_program({
      .name = name,
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = native,
      .expected_instructions = code.size(),
  });
}

void interleaved_four_member_lookup(engine::EngineHarness &harness) {
  constexpr rearrange::TableRegisters registers{
      .destination = 0U,
      .first = 1U,
      .indices = 5U,
  };
  constexpr auto lookup =
      rearrange::table_instruction(true, false, 4U, registers);
  constexpr std::array instructions{
      interleaving[0], interleaving[1], interleaving[2],
      interleaving[3], lookup,
  };
  const auto initial = rearrange::state(0x31U);
  auto before_lookup = initial;
  apply_interleaving(before_lookup);
  const auto expected = rearrange::expected_table_state(
      before_lookup, true, false, 4U, registers);
  check_program(harness, "jit-table-list-interleaved-four",
                instructions, initial, expected,
                rearrange::native_requirements());
}

void wrapped_alias_lookup(engine::EngineHarness &harness) {
  constexpr rearrange::TableRegisters registers{
      .destination = 31U,
      .first = 31U,
      .indices = 0U,
  };
  constexpr auto lookup =
      rearrange::table_instruction(true, true, 4U, registers);
  constexpr std::array instructions{lookup};
  const auto initial = rearrange::state(0x57U);
  const auto expected = rearrange::expected_table_state(
      initial, true, true, 4U, registers);
  check_program(harness, "jit-table-list-wrapped-alias",
                instructions, initial, expected,
                rearrange::native_requirements());
}

void interleaved_two_member_lut(engine::EngineHarness &harness) {
  constexpr auto form = lut::forms[2];
  constexpr auto instruction =
      lut::encode(form, 5U, 1U, 0U);
  constexpr std::array instructions{
      interleaving[0], interleaving[1], instruction,
  };
  const auto initial = lut::seeded_state(0x814U);
  auto before_lut = initial;
  apply_bitwise_xor(before_lut, 8U, 1U, 9U);
  apply_bitwise_xor(before_lut, 10U, 2U, 11U);
  const auto expected = lut::expected(before_lut, instruction);
  check_program(harness, "jit-table-list-interleaved-lut",
                instructions, initial, expected,
                lut::native_requirements());
}

} // namespace

int main() {
  engine::EngineHarness harness;
  interleaved_four_member_lookup(harness);
  wrapped_alias_lookup(harness);
  interleaved_two_member_lut(harness);
  harness.print_summary("arm64-jit-table-list-remap-runtime");
  return 0;
}
