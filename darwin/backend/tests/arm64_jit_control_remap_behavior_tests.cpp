// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstdint>

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

namespace {

[[nodiscard]] constexpr std::uint32_t
move_wide(unsigned destination, unsigned immediate) noexcept {
  return 0xd280'0000U | ((immediate & 0xffffU) << 5U) |
         (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
add_immediate(unsigned destination, unsigned source,
              unsigned immediate) noexcept {
  return 0x9100'0000U | ((immediate & 0xfffU) << 10U) |
         ((source & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
compare_branch(bool nonzero, unsigned source, unsigned displacement) noexcept {
  return (nonzero ? 0xb500'0000U : 0xb400'0000U) |
         ((displacement & 0x7ffffU) << 5U) | (source & 31U);
}

void compare_branch_commits_both_successors(
    engine_tests::EngineHarness &harness) {
  {
    const std::array code{
        engine_tests::ProgramWord{.offset = 0,
                                  .instruction = move_wide(5, 1)},
        engine_tests::ProgramWord{
            .offset = 4, .instruction = compare_branch(false, 5, 2)},
        engine_tests::ProgramWord{
            .offset = 8, .instruction = add_immediate(6, 5, 2)},
        engine_tests::ProgramWord{.offset = 12, .instruction = 0xd400'0001U},
    };
    arm64::CpuState initial{};
    auto expected = initial;
    expected.registers[5] = 1;
    expected.registers[6] = 3;
    expected.pc = engine_tests::engine_test_code_base + 16;
    harness.check_program({
        .name = "jit-resident-compare-branch-fallthrough",
        .code = code,
        .initial = initial,
        .expected = expected,
        .native = engine_tests::NativeRequirements::baseline(),
        .expected_instructions = code.size(),
    });
  }

  {
    const std::array code{
        engine_tests::ProgramWord{.offset = 0,
                                  .instruction = move_wide(5, 0)},
        engine_tests::ProgramWord{
            .offset = 4, .instruction = compare_branch(false, 5, 2)},
        engine_tests::ProgramWord{
            .offset = 8, .instruction = add_immediate(6, 6, 0x100)},
        engine_tests::ProgramWord{
            .offset = 12, .instruction = add_immediate(6, 5, 4)},
        engine_tests::ProgramWord{.offset = 16, .instruction = 0xd400'0001U},
    };
    arm64::CpuState initial{};
    initial.registers[6] = 9;
    auto expected = initial;
    expected.registers[5] = 0;
    expected.registers[6] = 4;
    expected.pc = engine_tests::engine_test_code_base + 20;
    harness.check_program({
        .name = "jit-resident-compare-branch-taken",
        .code = code,
        .initial = initial,
        .expected = expected,
        .native = engine_tests::NativeRequirements::baseline(),
        .expected_instructions = 4,
    });
  }
}

void pc_relative_result_remains_architectural(
    engine_tests::EngineHarness &harness) {
  const std::array code{
      engine_tests::ProgramWord{.offset = 0, .instruction = 0x1000'0005U},
      engine_tests::ProgramWord{
          .offset = 4, .instruction = add_immediate(6, 5, 48)},
      engine_tests::ProgramWord{.offset = 8, .instruction = 0xd400'0001U},
  };
  arm64::CpuState initial{};
  auto expected = initial;
  expected.registers[5] = engine_tests::engine_test_code_base;
  expected.registers[6] = engine_tests::engine_test_code_base + 48;
  expected.pc = engine_tests::engine_test_code_base + 12;
  harness.check_program({
      .name = "jit-resident-pc-relative-result",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::baseline(),
      .expected_instructions = code.size(),
  });
}

} // namespace

int main() {
  engine_tests::EngineHarness harness;
  compare_branch_commits_both_successors(harness);
  pc_relative_result_remains_architectural(harness);
  harness.print_summary("arm64-jit-control-remap-behavior");
  return 0;
}
