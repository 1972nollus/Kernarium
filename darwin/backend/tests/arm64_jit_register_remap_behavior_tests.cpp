// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"
#include "arm64_bfloat_arithmetic_test_support.hpp"
#include "arm64_bfloat_dot_test_support.hpp"
#include "arm64_bfloat_matrix_test_support.hpp"
#include "arm64_bfloat_multiply_test_support.hpp"
#include "arm64_half_dot_test_support.hpp"
#include "arm64_half_fused_long_test_support.hpp"
#include "arm64_half_matrix_test_support.hpp"
#include "arm64_simd_addp_scalar_test_support.hpp"
#include "arm64_simd_dot_test_support.hpp"
#include "arm64_simd_fp_indexed_test_support.hpp"
#include "arm64_simd_immediate_test_support.hpp"
#include "arm64_simd_indexed_test_support.hpp"
#include "arm64_simd_lane_move_test_support.hpp"
#include "arm64_simd_lut_test_support.hpp"
#include "arm64_simd_matrix_test_support.hpp"
#include "arm64_simd_rearrange_test_support.hpp"
#include "arm64_simd_reduce_test_support.hpp"
#include "arm64_simd_shift_immediate_test_support.hpp"
#include "arm64_simd_shift_left_long_test_support.hpp"
#include "arm64_simd_widening_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

namespace {

[[nodiscard]] constexpr std::uint32_t
three_register(std::uint32_t operation, unsigned destination, unsigned left,
               unsigned right) noexcept {
  return operation | ((right & 31U) << 16U) | ((left & 31U) << 5U) |
         (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
move_wide(unsigned destination, unsigned immediate) noexcept {
  return 0xd280'0000U | ((immediate & 0xffffU) << 5U) |
         (destination & 31U);
}

enum class SimdBitwiseOperation : unsigned {
  bit_and,
  bit_clear,
  bit_or,
  or_not,
  bit_xor,
  select,
  insert_true,
  insert_false,
};

[[nodiscard]] constexpr std::uint32_t
simd_bitwise(SimdBitwiseOperation operation, unsigned destination,
             unsigned left, unsigned right) noexcept {
  const auto encoded = static_cast<unsigned>(operation);
  return 0x4e20'1c00U | ((encoded & 3U) << 22U) |
         ((encoded & 4U) << 27U) | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
simd_modified_immediate(unsigned immediate, unsigned destination) noexcept {
  return 0x4f00'0400U | ((immediate & 0xe0U) << 11U) | (14U << 12U) |
         ((immediate & 0x1fU) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
vector_unsigned_memory(bool load, unsigned byte_offset, unsigned base,
                       unsigned transfer) noexcept {
  return 0x3d00'0000U | ((load ? 3U : 2U) << 22U) |
         (((byte_offset / 16U) & 0xfffU) << 10U) |
         ((base & 31U) << 5U) | (transfer & 31U);
}

[[nodiscard]] constexpr std::uint32_t
atomic_floating(unsigned base, unsigned source, unsigned result) noexcept {
  /* Binary32 FADD, relaxed ordering, returning the old memory value. */
  return 0xbc20'0000U | ((source & 31U) << 16U) |
         ((base & 31U) << 5U) | (result & 31U);
}

[[nodiscard]] constexpr unsigned
simd_bitwise_byte(SimdBitwiseOperation operation, unsigned old_destination,
                  unsigned left, unsigned right) noexcept {
  switch (operation) {
  case SimdBitwiseOperation::bit_and:
    return left & right;
  case SimdBitwiseOperation::bit_clear:
    return left & (right ^ 0xffU);
  case SimdBitwiseOperation::bit_or:
    return left | right;
  case SimdBitwiseOperation::or_not:
    return left | (right ^ 0xffU);
  case SimdBitwiseOperation::bit_xor:
    return left ^ right;
  case SimdBitwiseOperation::select:
    return (old_destination & left) |
           ((old_destination ^ 0xffU) & right);
  case SimdBitwiseOperation::insert_true:
    return (old_destination & (right ^ 0xffU)) | (left & right);
  case SimdBitwiseOperation::insert_false:
    return (old_destination & right) | (left & (right ^ 0xffU));
  }
  return 0;
}

void apply_simd_bitwise(arm64::CpuState &state,
                        SimdBitwiseOperation operation, unsigned destination,
                        unsigned left, unsigned right) {
  const auto old_destination = state.vectors[destination].bytes;
  const auto left_value = state.vectors[left].bytes;
  const auto right_value = state.vectors[right].bytes;
  for (unsigned byte = 0; byte < 16; ++byte) {
    state.vectors[destination].bytes[byte] = static_cast<std::byte>(
        simd_bitwise_byte(
            operation, std::to_integer<unsigned>(old_destination[byte]),
            std::to_integer<unsigned>(left_value[byte]),
            std::to_integer<unsigned>(right_value[byte])));
  }
}

void seed_vector_transition_state(arm64::CpuState &state) {
  for (unsigned byte = 0; byte < 16; ++byte) {
    state.vectors[3].bytes[byte] =
        static_cast<std::byte>(0x19U + byte * 7U);
    state.vectors[4].bytes[byte] =
        static_cast<std::byte>(0xe3U - byte * 5U);
    state.vectors[5].bytes[byte] =
        static_cast<std::byte>(0x52U ^ (byte * 11U));
    state.vectors[7].bytes[byte] =
        static_cast<std::byte>(0xa7U - byte * 3U);
  }
}

void write_vector_u64(arm64::CpuState &state, unsigned reg, unsigned lane,
                      std::uint64_t value) {
  for (unsigned byte = 0; byte < 8; ++byte)
    state.vectors[reg].bytes[lane * 8U + byte] =
        static_cast<std::byte>(value >> (byte * 8U));
}

[[nodiscard]] engine_tests::NativeRequirements
vector_memory_requirements() {
  return engine_tests::NativeRequirements::features({
      uml::backend::ArmFeature::advanced_simd,
      uml::backend::ArmFeature::floating_point,
  });
}

[[nodiscard]] engine_tests::NativeRequirements
atomic_floating_requirements() {
  return engine_tests::NativeRequirements::features({
      uml::backend::ArmFeature::advanced_simd,
      uml::backend::ArmFeature::floating_point,
      uml::backend::ArmFeature::lsfe,
  });
}

void ignore_counter_register(
    const arm64::CpuState &, const arm64::CpuState &expected_template,
    const arm64::CpuState &actual, std::string_view case_name,
    std::string_view engine, std::uint32_t instruction) {
  auto expected = expected_template;
  expected.registers[6] = actual.registers[6];
  engine_tests::require_architecture_equal(
      expected, actual, case_name, engine, instruction);
}

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
          uml::backend::ArmFeature::ls64),
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
          uml::backend::ArmFeature::advanced_simd),
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
          uml::backend::ArmFeature::advanced_simd),
      .expected_instructions = code.size(),
  });
}

void lane_reduce_and_shift_aliases_remain_architectural(
    engine_tests::EngineHarness &harness) {
  namespace lane = arm64_simd_lane_move_test;
  namespace reduce = arm64_simd_reduce_test;
  namespace addp = arm64_simd_addp_scalar_test;
  namespace shift = arm64_simd_shift_immediate_test;
  namespace long_shift = arm64_simd_shift_left_long_test;

  {
    constexpr lane::Registers registers{5U, 5U};
    constexpr unsigned immediate = 8U;
    const auto initial = lane::state(11U);
    harness.check_one(
        "jit-resident-DUP-general-numeric-alias",
        lane::duplicate_general_instruction(
            true, immediate, registers),
        initial,
        lane::expected_duplicate_general(
            initial, true, immediate, registers),
        lane::native_requirements());
  }
  {
    constexpr lane::Registers registers{6U, 6U};
    constexpr unsigned immediate = 8U;
    const auto initial = lane::state(12U);
    harness.check_one(
        "jit-resident-INS-general-numeric-alias",
        lane::insert_general_instruction(immediate, registers),
        initial,
        lane::expected_insert_general(initial, immediate, registers),
        lane::native_requirements());
  }
  {
    constexpr lane::Registers registers{7U, 7U};
    constexpr unsigned immediate = 8U;
    constexpr unsigned source_immediate = 8U;
    const auto initial = lane::state(13U);
    harness.check_one(
        "jit-resident-INS-element-vector-alias",
        lane::insert_element_instruction(
            immediate, source_immediate, registers),
        initial,
        lane::expected_insert_element(
            initial, immediate, source_immediate, registers),
        lane::native_requirements());
  }
  {
    constexpr lane::Registers registers{8U, 8U};
    constexpr unsigned immediate = 4U;
    const auto initial = lane::state(14U);
    harness.check_one(
        "jit-resident-DUP-element-vector-alias",
        lane::duplicate_element_instruction(
            false, true, immediate, registers),
        initial,
        lane::expected_duplicate_element(
            initial, false, true, immediate, registers),
        lane::native_requirements());
  }
  {
    constexpr unsigned reg = 9U;
    constexpr auto instruction =
        0x4e20'5800U | (reg << 5U) | reg; // CNT V9.16B,V9.16B
    const auto initial = lane::state(15U);
    auto expected = initial;
    for (unsigned byte = 0; byte < 16U; ++byte) {
      const auto value =
          std::to_integer<unsigned>(initial.vectors[reg].bytes[byte]);
      expected.vectors[reg].bytes[byte] =
          static_cast<std::byte>(std::popcount(value));
    }
    harness.check_one(
        "jit-resident-CNT-vector-alias", instruction, initial, expected,
        lane::native_requirements());
  }
  {
    constexpr unsigned reg = 9U;
    constexpr auto instruction =
        0x4e20'9800U | (reg << 5U) | reg; // CMEQ V9.16B,V9.16B,#0
    const auto initial = lane::state(16U);
    auto expected = initial;
    for (unsigned byte = 0; byte < 16U; ++byte)
      expected.vectors[reg].bytes[byte] =
          initial.vectors[reg].bytes[byte] == std::byte{}
              ? std::byte{0xff}
              : std::byte{};
    harness.check_one(
        "jit-resident-CMEQ-zero-vector-alias", instruction, initial,
        expected, lane::native_requirements());
  }
  {
    constexpr reduce::Registers registers{10U, 10U};
    constexpr bool q = true;
    constexpr bool unsigned_group = false;
    constexpr unsigned size = 0U;
    constexpr unsigned opcode = 27U;
    const auto initial = reduce::state(17U);
    harness.check_one(
        "jit-resident-ADDV-vector-alias",
        reduce::instruction(
            q, unsigned_group, size, opcode, registers),
        initial,
        reduce::expected_state(
            initial, q, unsigned_group, size, opcode, registers),
        reduce::native_requirements());
  }
  {
    constexpr unsigned reg = 11U;
    const auto initial = addp::state(18U);
    harness.check_one(
        "jit-resident-scalar-ADDP-vector-alias",
        addp::instruction(reg, reg), initial,
        addp::expected_state(initial, reg, reg),
        addp::native_requirements());
  }
  {
    constexpr unsigned reg = 12U;
    constexpr unsigned encoded = 56U;
    constexpr auto form = shift::Form::vector_128;
    constexpr auto operation = shift::Operation::ssra;
    const auto initial = shift::state(19U);
    harness.check_one(
        "jit-resident-SSRA-vector-alias",
        shift::instruction(form, operation, encoded, reg, reg),
        initial,
        shift::expected_state(
            initial, form, operation, encoded, reg, reg),
        shift::native_requirements());
  }
  {
    constexpr unsigned reg = 13U;
    constexpr unsigned encoded = 24U;
    constexpr auto form = shift::Form::vector_128;
    constexpr auto operation = shift::Operation::shrn;
    const auto initial = shift::state(20U);
    harness.check_one(
        "jit-resident-SHRN2-vector-alias",
        shift::instruction(form, operation, encoded, reg, reg),
        initial,
        shift::expected_state(
            initial, form, operation, encoded, reg, reg),
        shift::native_requirements());
  }
  {
    constexpr unsigned reg = 14U;
    constexpr unsigned encoded = 12U;
    constexpr unsigned source_bits = 8U;
    constexpr unsigned amount = encoded - source_bits;
    const auto initial = long_shift::state(21U);
    harness.check_one(
        "jit-resident-SSHLL2-vector-alias",
        long_shift::instruction(
            true, false, encoded, reg, reg),
        initial,
        long_shift::expected_state(
            initial, true, false, source_bits, amount, reg, reg),
        long_shift::native_requirements());
  }
}

void remaining_simd_arithmetic_aliases_remain_architectural(
    engine_tests::EngineHarness &harness) {
  namespace bfloat = arm64_bfloat_arithmetic_test;
  namespace bfloat_dot = arm64_bfloat_dot_test;
  namespace bfloat_matrix = arm64_bfloat_matrix_test;
  namespace bfloat_multiply = arm64_bfloat_multiply_test;
  namespace half_dot = arm64_half_dot_test;
  namespace half_fused = arm64_half_fused_long_test;
  namespace half_matrix = arm64_half_matrix_test;
  namespace dot = arm64_simd_dot_test;
  namespace fp_indexed = arm64_simd_fp_indexed_test;
  namespace indexed = arm64_simd_indexed_test;
  namespace lut = uml::backend::tests::simd_lut;
  namespace matrix = arm64_simd_matrix_test;
  namespace rearrange = arm64_simd_rearrange_test;
  namespace widening = arm64_simd_widening_test;

  {
    constexpr bfloat_dot::Form form{false, true, 0U};
    constexpr bfloat_dot::Registers registers{5U, 5U, 5U};
    auto initial = bfloat::state(31U);
    initial.fpcr = 0U;
    harness.check_one(
        "jit-resident-bfloat-dot-vector-alias",
        bfloat_dot::instruction(form, registers), initial,
        bfloat_dot::expected_state(initial, form, registers),
        bfloat_dot::native_requirements(initial));
  }
  {
    constexpr bfloat_dot::Form form{true, true, 3U};
    constexpr bfloat_dot::Registers registers{31U, 31U, 31U};
    auto initial = bfloat::state(32U);
    initial.fpcr = 0U;
    harness.check_one(
        "jit-resident-bfloat-dot-indexed-high-alias",
        bfloat_dot::instruction(form, registers), initial,
        bfloat_dot::expected_state(initial, form, registers),
        bfloat_dot::native_requirements(initial));
  }
  {
    constexpr half_dot::Form form{false, true, 0U};
    constexpr half_dot::Registers registers{6U, 6U, 6U};
    const auto initial = half_dot::state(33U);
    harness.check_one(
        "jit-resident-half-dot-vector-alias",
        half_dot::instruction(form, registers), initial,
        half_dot::expected_state(initial, form, registers),
        half_dot::native_requirements());
  }
  {
    constexpr half_dot::Form form{true, true, 3U};
    constexpr half_dot::Registers registers{31U, 31U, 31U};
    const auto initial = half_dot::state(34U);
    harness.check_one(
        "jit-resident-half-dot-indexed-high-alias",
        half_dot::instruction(form, registers), initial,
        half_dot::expected_state(initial, form, registers),
        half_dot::native_requirements());
  }
  {
    constexpr half_fused::Form form{
        false, false, false, true, 0U};
    constexpr half_fused::Registers registers{7U, 7U, 7U};
    const auto initial = half_fused::state(35U);
    harness.check_one(
        "jit-resident-half-fused-long-vector-alias",
        half_fused::instruction(form, registers), initial,
        half_fused::expected_state(initial, form, registers),
        half_fused::native_requirements());
  }
  {
    constexpr auto form =
        half_fused::indexed_form(false, false, true, 7U);
    constexpr half_fused::Registers registers{15U, 15U, 15U};
    const auto initial = half_fused::state(36U);
    harness.check_one(
        "jit-resident-half-fused-long-indexed-alias",
        half_fused::instruction(form, registers), initial,
        half_fused::expected_state(initial, form, registers),
        half_fused::native_requirements());
  }
  {
    constexpr bool widening_form = true;
    constexpr half_matrix::Registers registers{8U, 8U, 8U};
    const auto initial = half_matrix::state(37U);
    harness.check_one(
        "jit-resident-half-matrix-alias",
        half_matrix::instruction(widening_form, registers), initial,
        half_matrix::expected_state(initial, widening_form, registers),
        half_matrix::native_requirements(widening_form));
  }
  {
    constexpr auto form =
        bfloat_multiply::Form::vector_top;
    constexpr bfloat_multiply::Registers registers{9U, 9U, 9U};
    auto initial = bfloat::state(38U);
    initial.fpcr = 0U;
    harness.check_one(
        "jit-resident-bfloat-multiply-vector-alias",
        bfloat_multiply::instruction(form, 0U, registers), initial,
        bfloat_multiply::expected_state(
            initial, form, 0U, registers),
        bfloat_multiply::native_requirements(initial));
  }
  {
    constexpr auto form =
        bfloat_multiply::Form::indexed_top;
    constexpr bfloat_multiply::Registers registers{15U, 15U, 15U};
    auto initial = bfloat::state(39U);
    initial.fpcr = 0U;
    harness.check_one(
        "jit-resident-bfloat-multiply-indexed-alias",
        bfloat_multiply::instruction(form, 7U, registers), initial,
        bfloat_multiply::expected_state(
            initial, form, 7U, registers),
        bfloat_multiply::native_requirements(initial));
  }
  {
    constexpr bfloat_matrix::Registers registers{10U, 10U, 10U};
    auto initial = bfloat::state(40U);
    initial.fpcr = 0U;
    harness.check_one(
        "jit-resident-bfloat-matrix-alias",
        bfloat_matrix::instruction(registers), initial,
        bfloat_matrix::expected_state(initial, registers),
        bfloat_matrix::native_requirements(initial));
  }
  {
    constexpr auto form = dot::Form::vector;
    constexpr auto mode = dot::Mode::signed_signed;
    constexpr dot::Registers registers{11U, 11U, 11U};
    const auto initial = dot::state(41U);
    harness.check_one(
        "jit-resident-dot-vector-alias",
        dot::instruction(form, mode, true, registers, 0U), initial,
        dot::expected_state(
            initial, mode, true, false, registers, 0U),
        dot::native_requirements(mode));
  }
  {
    constexpr auto form = dot::Form::indexed;
    constexpr auto mode = dot::Mode::signed_signed;
    constexpr dot::Registers registers{12U, 12U, 12U};
    const auto initial = dot::state(42U);
    harness.check_one(
        "jit-resident-dot-indexed-alias",
        dot::instruction(form, mode, true, registers, 3U), initial,
        dot::expected_state(
            initial, mode, true, true, registers, 3U),
        dot::native_requirements(mode));
  }
  {
    constexpr auto mode = matrix::Mode::signed_signed;
    constexpr matrix::Registers registers{13U, 13U, 13U};
    const auto initial = matrix::state(43U);
    harness.check_one(
        "jit-resident-integer-matrix-alias",
        matrix::instruction(mode, registers), initial,
        matrix::expected_state(initial, mode, registers),
        matrix::native_requirements());
  }
  {
    constexpr indexed::Family family{
        indexed::Operation::mla, indexed::Encoding::indexed};
    constexpr indexed::Shape shape{2U, false, true};
    constexpr indexed::Registers registers{14U, 14U, 14U, 3U};
    const auto initial = indexed::state(44U);
    harness.check_one(
        "jit-resident-integer-indexed-word-alias",
        indexed::instruction(family, shape, registers), initial,
        indexed::expected_state(initial, family, shape, registers),
        indexed::native_requirements(family));
  }
  {
    constexpr indexed::Family family{
        indexed::Operation::mla, indexed::Encoding::indexed};
    constexpr indexed::Shape shape{1U, false, true};
    constexpr indexed::Registers registers{15U, 15U, 15U, 7U};
    const auto initial = indexed::state(45U);
    harness.check_one(
        "jit-resident-integer-indexed-half-alias",
        indexed::instruction(family, shape, registers), initial,
        indexed::expected_state(initial, family, shape, registers),
        indexed::native_requirements(family));
  }
  {
    constexpr auto operation =
        widening::Operation::absolute_accumulate;
    constexpr widening::Shape shape{1U, true};
    constexpr widening::Registers registers{16U, 16U, 16U};
    const auto initial = widening::state(46U);
    harness.check_one(
        "jit-resident-widening-accumulator-alias",
        widening::instruction(
            operation, false, shape, registers),
        initial,
        widening::expected_state(
            initial, operation, false, shape, registers),
        widening::native_requirements());
  }
  {
    constexpr auto operation = widening::Operation::add_long;
    constexpr widening::Shape shape{1U, false};
    constexpr widening::Registers registers{17U, 17U, 17U};
    const auto initial = widening::state(47U);
    harness.check_one(
        "jit-resident-widening-overwrite-alias",
        widening::instruction(
            operation, false, shape, registers),
        initial,
        widening::expected_state(
            initial, operation, false, shape, registers),
        widening::native_requirements());
  }
  {
    constexpr fp_indexed::Shape shape{2U, 4U, 4U, false, true};
    constexpr fp_indexed::Registers registers{18U, 18U, 18U};
    constexpr auto operation = fp_indexed::Operation::fmla;
    constexpr unsigned index = 3U;
    auto initial = fp_indexed::state(48U);
    fp_indexed::prepare_exact_inputs(
        initial, shape, registers, index);
    harness.check_one(
        "jit-resident-floating-indexed-word-alias",
        fp_indexed::instruction(
            operation, shape, registers.destination,
            registers.left, registers.element, index),
        initial,
        fp_indexed::expected_exact(
            initial, operation, shape, registers, index),
        fp_indexed::native_requirements(shape, initial.fpcr));
  }
  {
    constexpr fp_indexed::Shape shape{0U, 2U, 8U, false, true};
    constexpr fp_indexed::Registers registers{15U, 15U, 15U};
    constexpr auto operation = fp_indexed::Operation::fmla;
    constexpr unsigned index = 7U;
    auto initial = fp_indexed::state(49U);
    fp_indexed::prepare_exact_inputs(
        initial, shape, registers, index);
    harness.check_one(
        "jit-resident-floating-indexed-half-alias",
        fp_indexed::instruction(
            operation, shape, registers.destination,
            registers.left, registers.element, index),
        initial,
        fp_indexed::expected_exact(
            initial, operation, shape, registers, index),
        fp_indexed::native_requirements(shape, initial.fpcr));
  }
  {
    constexpr rearrange::PermuteRegisters registers{19U, 19U, 19U};
    constexpr unsigned opcode = 2U;
    const auto initial = rearrange::state(50U);
    harness.check_one(
        "jit-resident-permute-alias",
        rearrange::permute_instruction(
            true, 1U, opcode, registers),
        initial,
        rearrange::expected_permute_state(
            initial, true, 1U, opcode, registers),
        rearrange::native_requirements());
  }
  {
    constexpr rearrange::TableRegisters registers{20U, 20U, 20U};
    const auto initial = rearrange::state(51U);
    harness.check_one(
        "jit-resident-table-single-alias",
        rearrange::table_instruction(
            true, false, 1U, registers),
        initial,
        rearrange::expected_table_state(
            initial, true, false, 1U, registers),
        rearrange::native_requirements());
  }
  {
    constexpr rearrange::TableRegisters registers{21U, 21U, 21U};
    const auto initial = rearrange::state(52U);
    harness.check_one(
        "jit-resident-table-list-extend-alias",
        rearrange::table_instruction(
            true, true, 4U, registers),
        initial,
        rearrange::expected_table_state(
            initial, true, true, 4U, registers),
        rearrange::native_requirements());
  }
  {
    constexpr auto form = lut::forms[0];
    constexpr unsigned reg = 22U;
    constexpr auto instruction =
        lut::encode(form, reg, reg, reg);
    const auto initial = lut::seeded_state(53U);
    harness.check_one(
        "jit-resident-lut-single-alias", instruction, initial,
        lut::expected(initial, instruction),
        lut::native_requirements());
  }
  {
    constexpr auto form = lut::forms[2];
    constexpr unsigned reg = 23U;
    constexpr auto instruction =
        lut::encode(form, reg, reg, reg);
    const auto initial = lut::seeded_state(54U);
    harness.check_one(
        "jit-resident-lut-list-alias", instruction, initial,
        lut::expected(initial, instruction),
        lut::native_requirements());
  }
}

void dirty_vector_is_committed_before_memory_fault(
    engine_tests::EngineHarness &harness) {
  constexpr auto bit_xor = SimdBitwiseOperation::bit_xor;
  const std::array code{
      engine_tests::ProgramWord{
          .offset = 0,
          .instruction = simd_bitwise(bit_xor, 5, 3, 4)},
      engine_tests::ProgramWord{.offset = 4,
                                .instruction = 0xf940'00c7U},
      engine_tests::ProgramWord{.offset = 8, .instruction = 0xd400'0001U},
  };
  arm64::CpuState initial{};
  for (unsigned byte = 0; byte < 16; ++byte) {
    initial.vectors[3].bytes[byte] =
        static_cast<std::byte>(0x96U ^ (byte * 11U));
    initial.vectors[4].bytes[byte] =
        static_cast<std::byte>(0x4bU + byte * 9U);
    initial.vectors[5].bytes[byte] =
        static_cast<std::byte>(0xc3U - byte * 3U);
  }
  initial.registers[6] =
      engine_tests::engine_test_data_base + DARWIN_UML_GUEST_PAGE_SIZE;
  auto expected = initial;
  apply_simd_bitwise(expected, bit_xor, 5, 3, 4);
  expected.pc = engine_tests::engine_test_code_base + 4;
  harness.check_program({
      .name = "jit-resident-dirty-vector-memory-fault",
      .code = code,
      .initial = initial,
      .expected = expected,
      .event = engine_tests::ExpectedEvent::memory_fault,
      .native = engine_tests::NativeRequirements::features(
          uml::backend::ArmFeature::advanced_simd),
      .fault =
          {
              .address = initial.registers[6],
              .access = arm64::MemoryAccess::read,
          },
      .compare_fault = true,
      .expected_instructions = 1,
  });
}

void modified_immediate_overwrites_cached_vector(
    engine_tests::EngineHarness &harness) {
  constexpr unsigned destination = 5;
  constexpr unsigned immediate = 0xa6;
  const std::array code{
      engine_tests::ProgramWord{
          .offset = 0,
          .instruction = simd_bitwise(
              SimdBitwiseOperation::bit_xor, destination, 3, 4)},
      engine_tests::ProgramWord{
          .offset = 4,
          .instruction =
              simd_modified_immediate(immediate, destination)},
      engine_tests::ProgramWord{.offset = 8, .instruction = 0xd400'0001U},
  };
  arm64::CpuState initial{};
  seed_vector_transition_state(initial);
  auto expected = initial;
  expected.vectors[destination].bytes.fill(
      static_cast<std::byte>(immediate));
  expected.pc = engine_tests::engine_test_code_base + 12;
  harness.check_program({
      .name = "jit-cached-vector-modified-immediate-overwrite",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::features(
          uml::backend::ArmFeature::advanced_simd),
      .expected_instructions = code.size(),
  });
}

void modified_immediate_combines_with_cached_vector(
    engine_tests::EngineHarness &harness) {
  constexpr unsigned destination = 5U;
  constexpr unsigned or_immediate = 0x93U;
  constexpr unsigned clear_immediate = 0x56U;
  const std::array code{
      engine_tests::ProgramWord{
          .offset = 0,
          .instruction = simd_bitwise(
              SimdBitwiseOperation::bit_xor, destination, 3, 4)},
      engine_tests::ProgramWord{
          .offset = 4,
          .instruction = arm64_simd_immediate_test::instruction(
              true, false, 1U, false, or_immediate, destination)},
      engine_tests::ProgramWord{
          .offset = 8,
          .instruction = arm64_simd_immediate_test::instruction(
              true, true, 1U, false, clear_immediate, destination)},
      engine_tests::ProgramWord{.offset = 12, .instruction = 0xd400'0001U},
  };
  arm64::CpuState initial{};
  seed_vector_transition_state(initial);
  auto expected = initial;
  apply_simd_bitwise(
      expected, SimdBitwiseOperation::bit_xor, destination, 3, 4);
  expected = arm64_simd_immediate_test::expected_state(
      expected, true, false, 1U, false, or_immediate, destination);
  expected = arm64_simd_immediate_test::expected_state(
      expected, true, true, 1U, false, clear_immediate, destination);
  expected.pc = engine_tests::engine_test_code_base + 16;
  harness.check_program({
      .name = "jit-cached-vector-modified-immediate-combine",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::features(
          uml::backend::ArmFeature::advanced_simd),
      .expected_instructions = code.size(),
  });
}

void cached_vector_store_uses_resident_value(
    engine_tests::EngineHarness &harness) {
  constexpr unsigned destination = 5;
  constexpr unsigned transfer_offset = 64;
  const std::array code{
      engine_tests::ProgramWord{
          .offset = 0,
          .instruction = 0xf900'003fU}, // STR XZR, [X1]: warm write TLB
      engine_tests::ProgramWord{
          .offset = 4,
          .instruction = simd_bitwise(
              SimdBitwiseOperation::bit_xor, destination, 3, 4)},
      engine_tests::ProgramWord{
          .offset = 8,
          .instruction = vector_unsigned_memory(
              false, transfer_offset, 1, destination)},
      engine_tests::ProgramWord{.offset = 12, .instruction = 0xd400'0001U},
  };
  arm64::CpuState initial{};
  seed_vector_transition_state(initial);
  initial.registers[1] = engine_tests::engine_test_data_base;
  auto expected = initial;
  apply_simd_bitwise(
      expected, SimdBitwiseOperation::bit_xor, destination, 3, 4);
  expected.pc = engine_tests::engine_test_code_base + 16;

  std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> initial_page{};
  auto expected_page = initial_page;
  for (unsigned byte = 0; byte < 16; ++byte)
    expected_page[transfer_offset + byte] =
        expected.vectors[destination].bytes[byte];
  const std::array memory{
      engine_tests::MemoryRegion{
          .address = engine_tests::engine_test_data_base,
          .initial = std::span<const std::byte>{initial_page},
          .expected = std::span<const std::byte>{expected_page},
      },
  };
  harness.check_program({
      .name = "jit-cached-vector-warm-store",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = vector_memory_requirements(),
      .memory = memory,
      .expected_instructions = code.size(),
  });
}

void vector_load_overwrites_bound_destination(
    engine_tests::EngineHarness &harness) {
  constexpr unsigned destination = 5;
  constexpr unsigned transfer_offset = 64;
  const std::array code{
      engine_tests::ProgramWord{
          .offset = 0,
          .instruction = 0xf940'003fU}, // LDR XZR, [X1]: warm read TLB
      engine_tests::ProgramWord{
          .offset = 4,
          .instruction = simd_bitwise(
              SimdBitwiseOperation::bit_and, destination, 3, 4)},
      engine_tests::ProgramWord{
          .offset = 8,
          .instruction = vector_unsigned_memory(
              true, transfer_offset, 1, destination)},
      engine_tests::ProgramWord{
          .offset = 12,
          .instruction =
              simd_bitwise(SimdBitwiseOperation::bit_xor, 7, destination, 3)},
      engine_tests::ProgramWord{.offset = 16, .instruction = 0xd400'0001U},
  };
  arm64::CpuState initial{};
  seed_vector_transition_state(initial);
  initial.registers[1] = engine_tests::engine_test_data_base;

  std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> initial_page{};
  std::array<std::byte, 16> loaded{};
  for (unsigned byte = 0; byte < loaded.size(); ++byte) {
    loaded[byte] = static_cast<std::byte>(0xf1U - byte * 9U);
    initial_page[transfer_offset + byte] = loaded[byte];
  }
  const std::array memory{
      engine_tests::MemoryRegion{
          .address = engine_tests::engine_test_data_base,
          .initial = std::span<const std::byte>{initial_page},
      },
  };

  auto expected = initial;
  expected.vectors[destination].bytes = loaded;
  apply_simd_bitwise(
      expected, SimdBitwiseOperation::bit_xor, 7, destination, 3);
  expected.pc = engine_tests::engine_test_code_base + 20;
  harness.check_program({
      .name = "jit-bound-vector-warm-load",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = vector_memory_requirements(),
      .memory = memory,
      .expected_instructions = code.size(),
  });
}

void atomic_floating_alias_uses_old_resident_source(
    engine_tests::EngineHarness &harness) {
  constexpr unsigned transfer_offset = 128;
  constexpr unsigned source_and_result = 5;
  constexpr std::uint32_t one = 0x3f80'0000U;
  constexpr std::uint32_t two = 0x4000'0000U;
  constexpr std::uint32_t three = 0x4040'0000U;
  const std::array code{
      engine_tests::ProgramWord{
          .offset = 0,
          .instruction = simd_bitwise(
              SimdBitwiseOperation::bit_or, source_and_result,
              source_and_result, source_and_result)},
      engine_tests::ProgramWord{
          .offset = 4,
          .instruction = atomic_floating(
              1, source_and_result, source_and_result)},
      engine_tests::ProgramWord{
          .offset = 8,
          .instruction = simd_bitwise(
              SimdBitwiseOperation::bit_or, 7, source_and_result,
              source_and_result)},
      engine_tests::ProgramWord{.offset = 12, .instruction = 0xd400'0001U},
  };

  arm64::CpuState initial{};
  initial.registers[1] =
      engine_tests::engine_test_data_base + transfer_offset;
  initial.vectors[source_and_result].bytes.fill(std::byte{0xa5});
  for (unsigned byte = 0; byte < sizeof(two); ++byte)
    initial.vectors[source_and_result].bytes[byte] =
        static_cast<std::byte>(two >> (byte * 8U));

  std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> initial_page{};
  auto expected_page = initial_page;
  for (unsigned byte = 0; byte < sizeof(one); ++byte) {
    initial_page[transfer_offset + byte] =
        static_cast<std::byte>(one >> (byte * 8U));
    expected_page[transfer_offset + byte] =
        static_cast<std::byte>(three >> (byte * 8U));
  }

  auto expected = initial;
  expected.vectors[source_and_result].bytes.fill(std::byte{});
  expected.vectors[7].bytes.fill(std::byte{});
  for (unsigned byte = 0; byte < sizeof(one); ++byte) {
    const auto value = static_cast<std::byte>(one >> (byte * 8U));
    expected.vectors[source_and_result].bytes[byte] = value;
    expected.vectors[7].bytes[byte] = value;
  }
  expected.pc = engine_tests::engine_test_code_base + 16;

  const std::array memory{
      engine_tests::MemoryRegion{
          .address = engine_tests::engine_test_data_base,
          .initial = std::span<const std::byte>{initial_page},
          .expected = std::span<const std::byte>{expected_page},
      },
  };
  harness.check_program({
      .name = "jit-resident-atomic-floating-source-result-alias",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = atomic_floating_requirements(),
      .memory = memory,
      .expected_instructions = code.size(),
  });
}

void faulting_atomic_floating_does_not_publish_destination(
    engine_tests::EngineHarness &harness) {
  constexpr unsigned source = 5;
  constexpr unsigned destination = 6;
  const std::array code{
      engine_tests::ProgramWord{
          .offset = 0,
          .instruction = simd_bitwise(
              SimdBitwiseOperation::bit_or, source, source, source)},
      engine_tests::ProgramWord{
          .offset = 4,
          .instruction = atomic_floating(1, source, destination)},
      engine_tests::ProgramWord{.offset = 8, .instruction = 0xd400'0001U},
  };

  arm64::CpuState initial{};
  initial.registers[1] =
      engine_tests::engine_test_data_base + DARWIN_UML_GUEST_PAGE_SIZE;
  for (unsigned byte = 0; byte < 16; ++byte) {
    initial.vectors[source].bytes[byte] =
        static_cast<std::byte>(0x31U + byte * 7U);
    initial.vectors[destination].bytes[byte] =
        static_cast<std::byte>(0xe7U - byte * 9U);
  }
  auto expected = initial;
  expected.pc = engine_tests::engine_test_code_base + 4;
  harness.check_program({
      .name = "jit-resident-atomic-floating-fault-preserves-destination",
      .code = code,
      .initial = initial,
      .expected = expected,
      .event = engine_tests::ExpectedEvent::memory_fault,
      .native = atomic_floating_requirements(),
      .fault =
          {
              .address = initial.registers[1],
              .access = arm64::MemoryAccess::write,
          },
      .compare_fault = true,
      .expected_instructions = 1,
  });
}

void cross_domain_numeric_aliases_remain_independent(
    engine_tests::EngineHarness &harness) {
  const std::array code{
      engine_tests::ProgramWord{
          .offset = 0, .instruction = 0x9e67'00a5U}, // fmov d5, x5
      engine_tests::ProgramWord{
          .offset = 4, .instruction = 0x9e66'00a5U}, // fmov x5, d5
      engine_tests::ProgramWord{
          .offset = 8, .instruction = 0x9e62'00c6U}, // scvtf d6, x6
      engine_tests::ProgramWord{
          .offset = 12, .instruction = 0x9e78'00c6U}, // fcvtzs x6, d6
      engine_tests::ProgramWord{
          .offset = 16, .instruction = 0x1e7e'00e7U}, // fjcvtzs w7, d7
      engine_tests::ProgramWord{
          .offset = 20, .instruction = 0x9eaf'0108U}, // fmov v8.d[1], x8
      engine_tests::ProgramWord{
          .offset = 24, .instruction = 0x9eae'0129U}, // fmov x9, v9.d[1]
      engine_tests::ProgramWord{.offset = 28, .instruction = 0xd400'0001U},
  };
  arm64::CpuState initial{};
  initial.registers[5] = 0xc00a'0000'0000'0000ULL;
  initial.registers[6] = 42U;
  initial.registers[7] = 0xffff'ffff'ffff'ffffULL;
  initial.registers[8] = 0x1122'3344'5566'7788ULL;
  initial.registers[9] = 0xdead'beef'feed'faceULL;
  write_vector_u64(initial, 5, 0, 0x0102'0304'0506'0708ULL);
  write_vector_u64(initial, 5, 1, 0x1112'1314'1516'1718ULL);
  write_vector_u64(initial, 6, 0, 0x2122'2324'2526'2728ULL);
  write_vector_u64(initial, 6, 1, 0x3132'3334'3536'3738ULL);
  write_vector_u64(initial, 7, 0, 0x405e'c000'0000'0000ULL);
  write_vector_u64(initial, 7, 1, 0x4142'4344'4546'4748ULL);
  write_vector_u64(initial, 8, 0, 0xa1a2'a3a4'a5a6'a7a8ULL);
  write_vector_u64(initial, 8, 1, 0xb1b2'b3b4'b5b6'b7b8ULL);
  write_vector_u64(initial, 9, 0, 0xc1c2'c3c4'c5c6'c7c8ULL);
  write_vector_u64(initial, 9, 1, 0x8877'6655'4433'2211ULL);
  initial.nzcv = 0xa000'0000U;

  auto expected = initial;
  expected.vectors[5].bytes = {};
  write_vector_u64(expected, 5, 0, initial.registers[5]);
  expected.registers[5] = initial.registers[5];
  expected.vectors[6].bytes = {};
  write_vector_u64(expected, 6, 0, 0x4045'0000'0000'0000ULL);
  expected.registers[6] = 42U;
  expected.registers[7] = 123U;
  expected.nzcv = 0x4000'0000U;
  write_vector_u64(expected, 8, 1, initial.registers[8]);
  expected.registers[9] = 0x8877'6655'4433'2211ULL;
  expected.pc = engine_tests::engine_test_code_base + 32;

  harness.check_program({
      .name = "jit-cross-domain-numeric-register-aliases",
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::features({
          uml::backend::ArmFeature::floating_point,
          uml::backend::ArmFeature::advanced_simd,
          uml::backend::ArmFeature::jscvt,
      }),
      .expected_instructions = code.size(),
  });
}

} // namespace

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
  lane_reduce_and_shift_aliases_remain_architectural(harness);
  remaining_simd_arithmetic_aliases_remain_architectural(harness);
  dirty_vector_is_committed_before_memory_fault(harness);
  modified_immediate_overwrites_cached_vector(harness);
  modified_immediate_combines_with_cached_vector(harness);
  cached_vector_store_uses_resident_value(harness);
  vector_load_overwrites_bound_destination(harness);
  atomic_floating_alias_uses_old_resident_source(harness);
  faulting_atomic_floating_does_not_publish_destination(harness);
  cross_domain_numeric_aliases_remain_independent(harness);
  harness.print_summary("arm64-jit-register-remap-behavior");
  return 0;
}
