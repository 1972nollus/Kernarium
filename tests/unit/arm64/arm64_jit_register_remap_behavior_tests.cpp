// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_jit_register_remap_test_support.hpp"

namespace {

void dependent_integer_values_remain_architectural(
    engine_tests::EngineHarness &harness) {
  const std::array code{
      engine_tests::ProgramWord{.offset = 0,
                                .instruction = move_wide(14, 5)},
      engine_tests::ProgramWord{.offset = 4,
                                .instruction = move_wide(13, 7)},
      engine_tests::ProgramWord{
          .offset = 8,
          .instruction = three_register(0x8b00'0000U, 18, 14, 13)},
      engine_tests::ProgramWord{
          .offset = 12,
          .instruction = three_register(0xca00'0000U, 15, 18, 14)},
      engine_tests::ProgramWord{
          .offset = 16,
          .instruction = three_register(0x8b00'0000U, 14, 15, 13)},
      engine_tests::ProgramWord{
          .offset = 20,
          .instruction = three_register(0xcb00'0000U, 18, 14, 15)},
      engine_tests::ProgramWord{
          .offset = 24,
          .instruction = three_register(0xaa00'0000U, 15, 18, 13)},
      engine_tests::ProgramWord{.offset = 28, .instruction = 0xd400'0001U},
  };
  arm64::CpuState initial{};
  auto expected = initial;
  expected.registers[13] = 7;
  expected.registers[14] = 16;
  expected.registers[15] = 7;
  expected.registers[18] = 7;
  expected.pc = engine_tests::engine_test_code_base + 32;
  harness.check_program({
      .name = "jit-resident-dependent-integer-chain",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::baseline(),
      .expected_instructions = code.size(),
  });
}

void mixed_width_alias_clears_upper_bits(
    engine_tests::EngineHarness &harness) {
  const std::array code{
      engine_tests::ProgramWord{
          .offset = 0,
          .instruction = three_register(0x0b00'0000U, 5, 5, 6)},
      engine_tests::ProgramWord{
          .offset = 4,
          .instruction = three_register(0x8b00'0000U, 7, 5, 6)},
      engine_tests::ProgramWord{.offset = 8, .instruction = 0xd400'0001U},
  };
  arm64::CpuState initial{};
  initial.registers[5] = 0xfeed'face'ffff'ffffULL;
  initial.registers[6] = 1;
  auto expected = initial;
  expected.registers[5] = 0;
  expected.registers[7] = 1;
  expected.pc = engine_tests::engine_test_code_base + 12;
  harness.check_program({
      .name = "jit-resident-mixed-width-alias",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::baseline(),
      .expected_instructions = code.size(),
  });
}

void mixed_width_move_wide_sequence_preserves_x_aliases(
    engine_tests::EngineHarness &harness) {
  const std::array code{
      engine_tests::ProgramWord{.offset = 0,
                                .instruction = 0x929f'ffe4U},
      engine_tests::ProgramWord{.offset = 4,
                                .instruction = 0x12b2'0005U},
      engine_tests::ProgramWord{.offset = 8,
                                .instruction = 0xf2b2'0004U},
      engine_tests::ProgramWord{.offset = 12,
                                .instruction = 0x9100'b0a7U},
      engine_tests::ProgramWord{.offset = 16,
                                .instruction = 0x9100'9886U},
      engine_tests::ProgramWord{.offset = 20, .instruction = 0xd400'0001U},
  };
  arm64::CpuState initial{};
  auto expected = initial;
  expected.registers[4] = 0xffff'ffff'9000'0000ULL;
  expected.registers[5] = 0x0000'0000'6fff'ffffULL;
  expected.registers[6] = 0xffff'ffff'9000'0026ULL;
  expected.registers[7] = 0x0000'0000'7000'002bULL;
  expected.pc = engine_tests::engine_test_code_base + 24;
  harness.check_program({
      .name = "jit-resident-mixed-width-move-wide-sequence",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::baseline(),
      .expected_instructions = code.size(),
  });
}

void stack_pointer_is_data_not_host_sp(
    engine_tests::EngineHarness &harness) {
  const std::array code{
      engine_tests::ProgramWord{.offset = 0,
                                .instruction = 0x9100'43ffU},
      engine_tests::ProgramWord{.offset = 4,
                                .instruction = 0x9100'03e5U},
      engine_tests::ProgramWord{.offset = 8, .instruction = 0xd400'0001U},
  };
  arm64::CpuState initial{};
  initial.sp = engine_tests::engine_test_data_base + 0x800;
  auto expected = initial;
  expected.sp += 16;
  expected.registers[5] = expected.sp;
  expected.pc = engine_tests::engine_test_code_base + 12;
  harness.check_program({
      .name = "jit-resident-stack-pointer",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::baseline(),
      .expected_instructions = code.size(),
  });
}

void dirty_prefix_is_committed_before_memory_fault(
    engine_tests::EngineHarness &harness) {
  const std::array code{
      engine_tests::ProgramWord{.offset = 0,
                                .instruction = move_wide(5, 0x1234)},
      engine_tests::ProgramWord{.offset = 4,
                                .instruction = 0xf940'00c7U},
      engine_tests::ProgramWord{.offset = 8,
                                .instruction = 0x9100'04a5U},
      engine_tests::ProgramWord{.offset = 12, .instruction = 0xd400'0001U},
  };
  arm64::CpuState initial{};
  initial.registers[6] =
      engine_tests::engine_test_data_base + DARWIN_UML_GUEST_PAGE_SIZE;
  /*
   * x7 is the unexecuted load destination. Its distinctive old value proves
   * the destination is not marked dirty before the fault snapshots are
   * captured and then spuriously published by the cold exit.
   */
  initial.registers[7] = 0xfeed'1234'5678'abcdULL;
  auto expected = initial;
  expected.registers[5] = 0x1234;
  expected.pc = engine_tests::engine_test_code_base + 4;
  harness.check_program({
      .name = "jit-resident-dirty-prefix-memory-fault",
      .code = code,
      .initial = initial,
      .expected = expected,
      .event = engine_tests::ExpectedEvent::memory_fault,
      .native = engine_tests::NativeRequirements::baseline(),
      .fault =
          {
              .address = initial.registers[6],
              .access = arm64::MemoryAccess::read,
          },
      .compare_fault = true,
      .expected_instructions = 1,
  });
}

void dirty_prefix_survives_ls64_cold_fault(
    engine_tests::EngineHarness &harness) {
  const std::array code{
      engine_tests::ProgramWord{.offset = 0,
                                .instruction = move_wide(5, 0x5678)},
      engine_tests::ProgramWord{
          .offset = 4,
          .instruction = 0xf83f'd0c8U}, // ld64b x8, [x6]
      engine_tests::ProgramWord{.offset = 8, .instruction = 0xd400'0001U},
  };
  arm64::CpuState initial{};
  initial.registers[6] =
      engine_tests::engine_test_data_base + DARWIN_UML_GUEST_PAGE_SIZE;
  auto expected = initial;
  expected.registers[5] = 0x5678;
  expected.pc = engine_tests::engine_test_code_base + 4;
  harness.check_program({
      .name = "jit-resident-dirty-prefix-ls64-memory-fault",
      .code = code,
      .initial = initial,
      .expected = expected,
      .event = engine_tests::ExpectedEvent::memory_fault,
      .native = engine_tests::NativeRequirements::features(
          ::uml::cpu::ArmFeature::ls64),
      .fault =
          {
              .address = initial.registers[6],
              .access = arm64::MemoryAccess::read,
          },
      .compare_fault = true,
      .expected_instructions = 1,
  });
}

void native_counter_preserves_resident_values(
    engine_tests::EngineHarness &harness) {
  const std::array code{
      engine_tests::ProgramWord{.offset = 0,
                                .instruction = move_wide(5, 7)},
      engine_tests::ProgramWord{.offset = 4,
                                .instruction = 0xd53b'e046U},
      engine_tests::ProgramWord{.offset = 8,
                                .instruction = 0x9100'04a5U},
      engine_tests::ProgramWord{.offset = 12, .instruction = 0xd400'0001U},
  };
  arm64::CpuState initial{};
  auto expected = initial;
  expected.registers[5] = 8;
  expected.pc = engine_tests::engine_test_code_base + 16;
  harness.check_program({
      .name = "jit-resident-native-counter",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::baseline(),
      .validator = ignore_counter_register,
      .validator_relation_id = "ignore-host-counter-register/v1",
      .expected_instructions = code.size(),
  });
}

void dependent_vector_values_remain_architectural(
    engine_tests::EngineHarness &harness) {
  constexpr auto bit_and = SimdBitwiseOperation::bit_and;
  constexpr auto bit_clear = SimdBitwiseOperation::bit_clear;
  constexpr auto bit_or = SimdBitwiseOperation::bit_or;
  constexpr auto bit_xor = SimdBitwiseOperation::bit_xor;
  const std::array code{
      engine_tests::ProgramWord{
          .offset = 0,
          .instruction = simd_bitwise(bit_and, 18, 14, 13)},
      engine_tests::ProgramWord{
          .offset = 4,
          .instruction = simd_bitwise(bit_xor, 15, 18, 14)},
      engine_tests::ProgramWord{
          .offset = 8,
          .instruction = simd_bitwise(bit_or, 14, 15, 13)},
      engine_tests::ProgramWord{
          .offset = 12,
          .instruction = simd_bitwise(bit_clear, 18, 14, 15)},
      engine_tests::ProgramWord{
          .offset = 16,
          .instruction = simd_bitwise(bit_or, 15, 18, 13)},
      engine_tests::ProgramWord{.offset = 20, .instruction = 0xd400'0001U},
  };
  arm64::CpuState initial{};
  for (unsigned byte = 0; byte < 16; ++byte) {
    initial.vectors[13].bytes[byte] =
        static_cast<std::byte>(0x3cU + byte * 7U);
    initial.vectors[14].bytes[byte] =
        static_cast<std::byte>(0xa5U ^ (byte * 3U));
    initial.vectors[15].bytes[byte] =
        static_cast<std::byte>(0x5aU + byte);
    initial.vectors[18].bytes[byte] =
        static_cast<std::byte>(0xf0U - byte * 5U);
  }
  auto expected = initial;
  apply_simd_bitwise(expected, bit_and, 18, 14, 13);
  apply_simd_bitwise(expected, bit_xor, 15, 18, 14);
  apply_simd_bitwise(expected, bit_or, 14, 15, 13);
  apply_simd_bitwise(expected, bit_clear, 18, 14, 15);
  apply_simd_bitwise(expected, bit_or, 15, 18, 13);
  expected.pc = engine_tests::engine_test_code_base + 24;
  harness.check_program({
      .name = "jit-resident-dependent-vector-chain",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::features(
          ::uml::cpu::ArmFeature::advanced_simd),
      .expected_instructions = code.size(),
  });
}

void floating_aliases_remain_architectural(
    engine_tests::EngineHarness &harness) {
  /*
   * The pairwise result overwrites its own source and immediately becomes the
   * first input and destination of the reciprocal step. This catches both
   * source-before-destination allocation and stale CpuState staging.
   */
  const std::array code{
      engine_tests::ProgramWord{
          .offset = 0,
          .instruction =
              0x7e70'd800U | (5U << 5U) | 5U}, // faddp d5, v5.2d
      engine_tests::ProgramWord{
          .offset = 4,
          .instruction = 0x5e60'fc00U | (6U << 16U) |
                         (5U << 5U) | 5U}, // frecps d5, d5, d6
      engine_tests::ProgramWord{
          .offset = 8,
          .instruction = simd_bitwise(
              SimdBitwiseOperation::bit_or, 7, 5, 5)},
      engine_tests::ProgramWord{.offset = 12, .instruction = 0xd400'0001U},
  };

  arm64::CpuState initial{};
  write_vector_u64(initial, 5, 0, 0x3ff0'0000'0000'0000ULL);
  write_vector_u64(initial, 5, 1, 0x4000'0000'0000'0000ULL);
  write_vector_u64(initial, 6, 0, 0x3fe0'0000'0000'0000ULL);
  write_vector_u64(initial, 6, 1, 0xdead'beef'feed'faceULL);
  initial.vectors[7].bytes.fill(std::byte{0xa5});

  auto expected = initial;
  expected.vectors[5].bytes = {};
  write_vector_u64(expected, 5, 0, 0x3fe0'0000'0000'0000ULL);
  expected.vectors[7].bytes = expected.vectors[5].bytes;
  expected.pc = engine_tests::engine_test_code_base + 16;
  harness.check_program({
      .name = "jit-resident-floating-source-destination-aliases",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::features(
          ::uml::cpu::ArmFeature::advanced_simd),
      .expected_instructions = code.size(),
  });
}


} // namespace

void run_register_remap_simd_behavior_tests(
    engine_tests::EngineHarness &harness);

int main() {
  engine_tests::EngineHarness harness;
  dependent_integer_values_remain_architectural(harness);
  mixed_width_alias_clears_upper_bits(harness);
  mixed_width_move_wide_sequence_preserves_x_aliases(harness);
  stack_pointer_is_data_not_host_sp(harness);
  dirty_prefix_is_committed_before_memory_fault(harness);
  dirty_prefix_survives_ls64_cold_fault(harness);
  native_counter_preserves_resident_values(harness);
  dependent_vector_values_remain_architectural(harness);
  floating_aliases_remain_architectural(harness);
  run_register_remap_simd_behavior_tests(harness);
  harness.print_summary("arm64-jit-register-remap-behavior");
  return 0;
}
