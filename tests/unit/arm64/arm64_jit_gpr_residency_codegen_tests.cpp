// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/jit/jit_test_api.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <span>

namespace jit_test = uml::cpu::arm64::jit::testing;

namespace {

constexpr std::uint32_t register_mask =
    (31U << 16U) | (31U << 5U) | 31U;
constexpr std::uint64_t code_base = 0x4f80'0000ULL;

[[noreturn]] void
fail(const char *message,
     const std::source_location location = std::source_location::current()) {
  std::fprintf(stderr, "%s at %s:%u\n", message, location.file_name(),
               location.line());
  std::abort();
}

void require(bool condition, const char *message,
             const std::source_location location =
                 std::source_location::current()) {
  if (!condition)
    fail(message, location);
}

[[nodiscard]] constexpr std::uint32_t
add_register(unsigned destination, unsigned left,
             unsigned right) noexcept {
  return 0x8b00'0000U | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr bool
is_cpu_state_gpr_load(std::uint32_t word) noexcept {
  return (word & 0xffc0'03e0U) == 0xf940'01e0U;
}

[[nodiscard]] constexpr bool
is_cpu_state_gpr_store(std::uint32_t word) noexcept {
  return (word & 0xffc0'03e0U) == 0xf900'01e0U;
}

[[nodiscard]] constexpr unsigned destination(std::uint32_t word) noexcept {
  return word & 31U;
}

[[nodiscard]] constexpr unsigned left(std::uint32_t word) noexcept {
  return (word >> 5U) & 31U;
}

[[nodiscard]] constexpr unsigned right(std::uint32_t word) noexcept {
  return (word >> 16U) & 31U;
}

[[nodiscard]] std::span<const std::uint32_t>
hot_code(const jit_test::CompiledCode &compiled) {
  require(compiled.hot_words <= compiled.words.size(),
          "JIT GPR hot boundary exceeds generated code");
  return std::span<const std::uint32_t>{compiled.words}
      .first(compiled.hot_words);
}

[[nodiscard]] std::size_t
count_if(std::span<const std::uint32_t> words,
         bool (*predicate)(std::uint32_t)) {
  std::size_t result{};
  for (const auto word : words)
    result += predicate(word);
  return result;
}

void require_same_metadata(const jit_test::CompiledCode &left_code,
                           const jit_test::CompiledCode &right_code) {
  require(left_code.guest_instructions ==
                  right_code.guest_instructions &&
              left_code.internal_entry_word ==
                  right_code.internal_entry_word &&
              left_code.memory_write_mask ==
                  right_code.memory_write_mask &&
              left_code.direct_memory_write_mask ==
                  right_code.direct_memory_write_mask &&
              left_code.accesses_fp_state ==
                  right_code.accesses_fp_state &&
              left_code.link_target_safe ==
                  right_code.link_target_safe &&
              left_code.native_fp == right_code.native_fp &&
              left_code.native_fpmr == right_code.native_fpmr &&
              left_code.guest_illegal == right_code.guest_illegal,
          "GPR residency planning changed architectural metadata");
}

constexpr std::array pressure_guest{
    add_register(0U, 1U, 1U),
    add_register(2U, 3U, 3U),
    add_register(4U, 5U, 5U),
    add_register(6U, 7U, 7U),
    add_register(8U, 9U, 9U),
    add_register(8U, 8U, 9U),
    add_register(8U, 8U, 9U),
    add_register(8U, 8U, 9U),
};

void repeated_late_pair_replaces_single_use_bindings() {
  const auto planned =
      jit_test::compile_block(code_base, pressure_guest);
  const auto unplanned =
      jit_test::compile_block_without_gpr_residency_planning(
          code_base, pressure_guest);
  require(planned.guest_instructions == pressure_guest.size(),
          "JIT split the GPR pressure block");
  require(!unplanned.gpr_residency_planned,
          "test-only baseline unexpectedly planned GPR residency");
  require_same_metadata(planned, unplanned);

  require(planned.gpr_residency_planned,
          "JIT did not retain the profitable GPR plan");
  require(planned.gpr_estimated_avoided_accesses == 9U,
          "GPR planner reported the wrong avoided-access estimate");
  require(planned.hot_words + 9U == unplanned.hot_words,
          "GPR planner did not remove exactly nine hot instructions");
  require(planned.words.size() < unplanned.words.size(),
          "GPR planner did not reduce total generated code");

  const auto planned_hot = hot_code(planned);
  const auto unplanned_hot = hot_code(unplanned);
  require(count_if(planned_hot, &is_cpu_state_gpr_load) + 6U ==
              count_if(unplanned_hot, &is_cpu_state_gpr_load),
          "GPR planner did not remove six repeated state loads");
  require(count_if(planned_hot, &is_cpu_state_gpr_store) + 3U ==
              count_if(unplanned_hot, &is_cpu_state_gpr_store),
          "GPR planner did not remove three intermediate state stores");

  std::array<std::uint32_t, 4> late_native{};
  std::size_t late_count{};
  for (const auto word : planned_hot) {
    if ((word & ~register_mask) !=
        (pressure_guest[4] & ~register_mask))
      continue;
    if (destination(word) != 17U)
      continue;
    require(late_count < late_native.size(),
            "JIT emitted too many late resident additions");
    late_native[late_count++] = word;
  }
  require(late_count == late_native.size(),
          "JIT omitted a late resident addition");
  require(left(late_native[0]) == 13U &&
              right(late_native[0]) == 13U,
          "first late addition did not use the selected host pair");
  for (std::size_t index = 1; index < late_native.size(); ++index)
    require(left(late_native[index]) == 17U &&
                right(late_native[index]) == 13U,
            "late dependency was not kept in its selected host register");
}

void low_pressure_code_stays_byte_identical() {
  constexpr std::array guest{
      add_register(0U, 1U, 1U),
      add_register(0U, 0U, 1U),
      add_register(0U, 0U, 1U),
  };
  const auto planned =
      jit_test::compile_block(code_base + 0x1000U, guest);
  const auto unplanned =
      jit_test::compile_block_without_gpr_residency_planning(
          code_base + 0x1000U, guest);
  require(!planned.gpr_residency_planned,
          "low-pressure block retained a GPR plan");
  require(planned.words == unplanned.words &&
              planned.hot_words == unplanned.hot_words,
          "low-pressure GPR planning changed generated code");
  require_same_metadata(planned, unplanned);
}

void no_benefit_pressure_keeps_first_use_allocation() {
  constexpr std::array guest{
      add_register(0U, 1U, 1U),
      add_register(2U, 3U, 3U),
      add_register(4U, 5U, 5U),
      add_register(6U, 7U, 7U),
      add_register(8U, 9U, 9U),
  };
  const auto planned =
      jit_test::compile_block(code_base + 0x2000U, guest);
  const auto unplanned =
      jit_test::compile_block_without_gpr_residency_planning(
          code_base + 0x2000U, guest);
  require(!planned.gpr_residency_planned,
          "zero-benefit pressure retained a GPR plan");
  require(planned.words == unplanned.words &&
              planned.hot_words == unplanned.hot_words,
          "zero-benefit pressure changed first-use allocation");
  require_same_metadata(planned, unplanned);
}

void fetched_words_after_terminal_do_not_affect_selection() {
  constexpr std::uint32_t system_call = 0xd400'0001U;
  constexpr std::array prefix{
      add_register(0U, 1U, 1U),
      system_call,
  };
  constexpr std::array fetched{
      prefix[0], prefix[1],
      pressure_guest[0], pressure_guest[1],
      pressure_guest[2], pressure_guest[3],
      pressure_guest[4], pressure_guest[5],
      pressure_guest[6], pressure_guest[7],
  };
  const auto expected =
      jit_test::compile_block(code_base + 0x3000U, prefix);
  const auto actual =
      jit_test::compile_block(code_base + 0x3000U, fetched);
  require(!actual.gpr_residency_planned,
          "future fetched words selected a GPR plan");
  require(actual.words == expected.words &&
              actual.hot_words == expected.hot_words,
          "future fetched words changed a terminal GPR prefix");
  require_same_metadata(actual, expected);
}

void linked_pressure_block_retains_the_smaller_plan() {
  constexpr std::uint32_t direct_branch = 0x1400'0001U;
  constexpr std::array guest{
      pressure_guest[0], pressure_guest[1],
      pressure_guest[2], pressure_guest[3],
      pressure_guest[4], pressure_guest[5],
      pressure_guest[6], pressure_guest[7],
      direct_branch,
  };
  const auto compiled =
      jit_test::compile_linked_block(code_base + 0x4000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "linked pressure block did not compile completely");
  require(compiled.gpr_residency_planned,
          "linked pressure block rejected its smaller GPR plan");
  require(compiled.gpr_estimated_avoided_accesses == 9U,
          "linked pressure block changed the GPR traffic estimate");
  require(!compiled.words.empty() && compiled.link_target_safe,
          "linked pressure plan corrupted link metadata");
}

} // namespace

int main() {
  repeated_late_pair_replaces_single_use_bindings();
  low_pressure_code_stays_byte_identical();
  no_benefit_pressure_keeps_first_use_allocation();
  fetched_words_after_terminal_do_not_affect_selection();
  linked_pressure_block_retains_the_smaller_plan();
  return 0;
}
