// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/jit/jit_test_api.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <source_location>
#include <span>

namespace jit_test = uml::cpu::arm64::jit::testing;

namespace {

[[noreturn]] void fail(
    const char *message,
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

[[nodiscard]] std::span<const std::uint32_t>
hot_code(const jit_test::CompiledCode &compiled) {
  require(compiled.hot_words <= compiled.words.size(),
          "hot boundary exceeds generated code");
  return std::span<const std::uint32_t>{compiled.words}.first(
      compiled.hot_words);
}

[[nodiscard]] std::size_t count_word(
    std::span<const std::uint32_t> words, std::uint32_t expected) {
  return static_cast<std::size_t>(
      std::count(words.begin(), words.end(), expected));
}

[[nodiscard]] std::size_t successful_path_words(
    std::span<const std::uint32_t> hot, std::size_t first) {
  require(first < hot.size(), "successful linked path starts outside hot code");
  const auto begin = hot.begin() + static_cast<std::ptrdiff_t>(first);
  const auto end = std::find(begin, hot.end(), 0xd61f'01a0U); // br x13
  require(end != hot.end(), "successful linked path has no host BR");
  return static_cast<std::size_t>(end - begin) + 1;
}

struct LiteralLoad final {
  std::size_t source;
  std::size_t target;
  std::uint64_t value;
};

[[nodiscard]] std::int64_t sign_extend(std::uint32_t value,
                                       unsigned bits) noexcept {
  const auto shift = 32U - bits;
  return static_cast<std::int32_t>(value << shift) >> shift;
}

[[nodiscard]] std::optional<std::int64_t>
immediate_branch_target(std::uint32_t word, std::size_t source) {
  std::int64_t displacement{};
  if ((word & 0x7c00'0000U) == 0x1400'0000U)
    displacement = sign_extend(word & 0x03ff'ffffU, 26);
  else if ((word & 0xff00'0010U) == 0x5400'0000U)
    displacement = sign_extend((word >> 5U) & 0x7ffffU, 19);
  else if ((word & 0x7e00'0000U) == 0x3400'0000U)
    displacement = sign_extend((word >> 5U) & 0x7ffffU, 19);
  else if ((word & 0x7e00'0000U) == 0x3600'0000U)
    displacement = sign_extend((word >> 5U) & 0x3fffU, 14);
  else
    return std::nullopt;
  return static_cast<std::int64_t>(source) + displacement;
}

[[nodiscard]] std::size_t
require_unique_instruction(std::span<const std::uint32_t> words,
                           std::uint32_t mask, std::uint32_t value,
                           const char *message) {
  std::optional<std::size_t> found;
  for (std::size_t index = 0; index < words.size(); ++index) {
    if ((words[index] & mask) != value)
      continue;
    require(!found.has_value(), message);
    found = index;
  }
  require(found.has_value(), message);
  return *found;
}

[[nodiscard]] std::size_t
require_branch_target(std::span<const std::uint32_t> words,
                      std::size_t source, const char *message) {
  require(source < words.size(), message);
  const auto target = immediate_branch_target(words[source], source);
  require(target.has_value() && *target >= 0 &&
              static_cast<std::size_t>(*target) < words.size(),
          message);
  return static_cast<std::size_t>(*target);
}

[[nodiscard]] bool is_move_wide_to(std::uint32_t word,
                                   unsigned destination) noexcept {
  const auto shape = word & 0xff80'001fU;
  return shape == (0xd280'0000U | (destination & 31U)) ||
         shape == (0xf280'0000U | (destination & 31U));
}

[[nodiscard]] LiteralLoad
require_single_literal_pointer(const jit_test::CompiledCode &compiled,
                               unsigned destination) {
  std::optional<LiteralLoad> found;
  for (std::size_t source = 0; source < compiled.hot_words; ++source) {
    const auto word = compiled.words[source];
    if ((word & 0xff00'001fU) !=
        (0x5800'0000U | (destination & 31U)))
      continue;
    const auto displacement =
        sign_extend((word >> 5U) & 0x7ffffU, 19);
    const auto signed_target =
        static_cast<std::int64_t>(source) + displacement;
    require(signed_target >= 0,
            "stable pointer literal targets before generated storage");
    const auto target = static_cast<std::size_t>(signed_target);
    require(target + 1U < compiled.words.size(),
            "stable pointer literal targets outside generated storage");
    require(!found.has_value(),
            "stable pointer uses more than one literal load");
    found = LiteralLoad{
        .source = source,
        .target = target,
        .value = static_cast<std::uint64_t>(compiled.words[target]) |
                 (static_cast<std::uint64_t>(compiled.words[target + 1U])
                  << 32U),
    };
  }
  require(found.has_value(),
          "stable pointer does not use one PC-relative literal load");
  require(found->target >= compiled.hot_words,
          "stable pointer data overlaps the hot instruction stream");
  require((found->target & 1U) == 0U,
          "stable pointer data is not eight-byte aligned");
  require(found->value != 0 &&
              (found->value % alignof(std::uintptr_t)) == 0,
          "stable pointer literal is not a valid aligned host pointer");

  /*
   * Other translation-resolved constants may now share the immutable tail,
   * so this pointer need not be the final pool entry.  Every entry still has
   * to remain outside the executable hot stream and outside all branch
   * targets.
   */
  for (std::size_t source = 0; source < found->target; ++source) {
    const auto target =
        immediate_branch_target(compiled.words[source], source);
    if (!target)
      continue;
    require(*target != static_cast<std::int64_t>(found->target) &&
                *target !=
                    static_cast<std::int64_t>(found->target + 1U),
            "generated branch targets stable pointer data");
  }
  for (const auto word : hot_code(compiled))
    require(!is_move_wide_to(word, destination),
            "hot path still materializes the stable pointer with MOV-wide");
  return *found;
}

void require_only_native_br_x13(std::span<const std::uint32_t> hot,
                                std::size_t expected) {
  std::size_t branches = 0;
  for (const auto word : hot) {
    if ((word & 0xffff'fc1fU) != 0xd61f'0000U)
      continue;
    ++branches;
    require(((word >> 5U) & 31U) == 13U,
            "native linked branch does not use host-selected x13");
  }
  require(branches == expected,
          "linked path has an unexpected native branch count");
}

void out_of_range_literal_uses_cold_absolute_fallback() {
  constexpr std::uintptr_t pointer = 0x1234'5678'9abc'def0ULL;
  const auto words =
      jit_test::compile_stable_pointer_load_for_testing(pointer, 1);
  require(words.size() == 7,
          "out-of-range pointer fallback changed size");
  const auto entry_target = immediate_branch_target(words[0], 0);
  require(entry_target && *entry_target == 2,
          "out-of-range pointer does not branch to its cold fallback");
  require(words[1] == 0xd65f'03c0U,
          "pointer fallback does not resume at the original continuation");
  std::size_t materialization_words = 0;
  std::size_t literal_loads = 0;
  for (const auto word : words) {
    materialization_words += is_move_wide_to(word, 12);
    literal_loads +=
        (word & 0xff00'001fU) == (0x5800'0000U | 12U);
    require((word & 0xffff'fc1fU) != 0xd61f'0000U,
            "pointer fallback introduces a register branch");
  }
  require(materialization_words == 4 && literal_loads == 0,
          "out-of-range pointer does not use exact MOV-wide fallback");
  const auto resume_target =
      immediate_branch_target(words.back(), words.size() - 1U);
  require(resume_target && *resume_target == 1,
          "pointer fallback does not branch back to the hot continuation");
}

void two_pointer_literals_use_the_fixed_capacity_pool() {
  constexpr std::uintptr_t first = 0x1234'5678'9abc'def0ULL;
  constexpr std::uintptr_t second = 0xfedc'ba98'7654'3210ULL;
  const auto words = jit_test::compile_two_stable_pointer_loads_for_testing(
      first, second, std::size_t{1} << 18U);
  constexpr std::array expected{
      0x5800'008cU, // ldr x12, literal at word 4
      0x5800'00adU, // ldr x13, literal at word 6
      0xd65f'03c0U, // ret
      0x0000'0000U, // eight-byte pool alignment
      0x9abc'def0U,
      0x1234'5678U,
      0x7654'3210U,
      0xfedc'ba98U,
  };
  require(std::equal(expected.begin(), expected.end(), words.begin(),
                     words.end()),
          "two-pointer literal pool changed its exact layout");
}

void two_out_of_range_literals_use_independent_cold_fallbacks() {
  constexpr std::uintptr_t first = 0x1234'5678'9abc'def0ULL;
  constexpr std::uintptr_t second = 0xfedc'ba98'7654'3210ULL;
  const auto words = jit_test::compile_two_stable_pointer_loads_for_testing(
      first, second, 1);
  require(words.size() == 13,
          "two-pointer cold fallback changed size");
  require(immediate_branch_target(words[0], 0) == 3 &&
              immediate_branch_target(words[1], 1) == 8,
          "two-pointer fallbacks do not have independent entry branches");
  require(words[2] == 0xd65f'03c0U,
          "two-pointer fallbacks displaced the hot continuation");
  require(immediate_branch_target(words[7], 7) == 1 &&
              immediate_branch_target(words[12], 12) == 2,
          "two-pointer fallbacks do not resume at their own continuations");
  std::size_t first_materialization_words = 0;
  std::size_t second_materialization_words = 0;
  for (const auto word : words) {
    first_materialization_words += is_move_wide_to(word, 12);
    second_materialization_words += is_move_wide_to(word, 13);
    require((word & 0xff00'001fU) != (0x5800'0000U | 12U) &&
                (word & 0xff00'001fU) != (0x5800'0000U | 13U),
            "out-of-range two-pointer fallback retains a literal load");
    require((word & 0xffff'fc1fU) != 0xd61f'0000U,
            "two-pointer fallback introduces a register branch");
  }
  require(first_materialization_words == 4 &&
              second_materialization_words == 4,
          "two-pointer fallback does not materialize both exact pointers");
}

void frame_shape_saves_and_restores_host_chain_registers() {
  constexpr std::uint64_t pc = 0x5a00'0000;
  constexpr std::array guest{0xd400'0001U}; // svc #0
  const auto compiled = jit_test::compile_block(pc, guest);
  constexpr std::array control_prologue{
      0xa9bc'7bfdU, // stp x29, x30, [sp, #-64]!
      0x9100'03fdU, // mov x29, sp
      0xa901'07e0U, // stp x0, x1, [sp, #16]
      0xa902'57e2U, // stp x2, x21, [sp, #32]
      0xa903'53f3U, // stp x19, x20, [sp, #48]
      0xaa00'03efU, // mov x15, x0
      0xaa01'03eeU, // mov x14, x1
      0xaa02'03f3U, // mov x19, x2
      0xd282'0014U, // mov x20, #4096
      0x3940'6275U, // ldrb w21, [x19, #24]
  };
  constexpr std::array bank_prologue{
      0xa9bb'7bfdU, // stp x29, x30, [sp, #-80]!
      0x9100'03fdU, // mov x29, sp
      0xa901'07e0U, // stp x0, x1, [sp, #16]
      0xa902'57e2U, // stp x2, x21, [sp, #32]
      0xa903'53f3U, // stp x19, x20, [sp, #48]
      0xa904'5ff6U, // stp x22, x23, [sp, #64]
      0xaa00'03efU, // mov x15, x0
      0xaa01'03eeU, // mov x14, x1
      0xaa02'03f3U, // mov x19, x2
      0xd282'0014U, // mov x20, #4096
      0x3940'6275U, // ldrb w21, [x19, #24]
      0xa944'5df6U, // ldp x22, x23, [x15, #64]
  };
  constexpr std::array control_epilogue{
      0xf900'0274U, // str x20, [x19]
      0xa943'53f3U, // ldp x19, x20, [sp, #48]
      0xf940'17f5U, // ldr x21, [sp, #40]
      0xa8c4'7bfdU, // ldp x29, x30, [sp], #64
      0xd65f'03c0U, // ret
  };
  constexpr std::array bank_epilogue{
      0xf900'0274U, // str x20, [x19]
      0xa944'5ff6U, // ldp x22, x23, [sp, #64]
      0xa943'53f3U, // ldp x19, x20, [sp, #48]
      0xf940'17f5U, // ldr x21, [sp, #40]
      0xa8c5'7bfdU, // ldp x29, x30, [sp], #80
      0xd65f'03c0U, // ret
  };

  require(jit_test::persistent_linked_block_abi_enabled(),
          "persistent linked-block ABI is not the translation default");
  if (jit_test::linked_x8_x9_bank_enabled()) {
    require(compiled.internal_entry_word == bank_prologue.size(),
            "bank internal entry does not skip setup");
    require(std::equal(bank_prologue.begin(), bank_prologue.end(),
                       compiled.words.begin()),
            "bank persistent linked-block prologue changed");
    require(std::search(compiled.words.begin(), compiled.words.end(),
                        bank_epilogue.begin(), bank_epilogue.end()) !=
                compiled.words.end(),
            "bank persistent linked-block epilogue changed");
  } else {
    require(compiled.internal_entry_word == control_prologue.size(),
            "control internal entry does not skip setup");
    require(std::equal(control_prologue.begin(), control_prologue.end(),
                       compiled.words.begin()),
            "control persistent linked-block prologue changed");
    require(std::search(compiled.words.begin(), compiled.words.end(),
                        control_epilogue.begin(), control_epilogue.end()) !=
                compiled.words.end(),
            "control persistent linked-block epilogue changed");
  }

  const auto memory_abi =
      jit_test::compile_linked_block_memory_abi(pc, guest);
  constexpr std::array memory_prologue{
      0xa9bd'7bfdU, // stp x29, x30, [sp, #-48]!
      0x9100'03fdU, // mov x29, sp
      0xa901'07e0U, // stp x0, x1, [sp, #16]
      0xf900'13e2U, // str x2, [sp, #32]
      0xaa00'03efU, // mov x15, x0
      0xaa01'03eeU, // mov x14, x1
  };
  require(memory_abi.internal_entry_word == memory_prologue.size() &&
              std::equal(memory_prologue.begin(), memory_prologue.end(),
                         memory_abi.words.begin()),
          "nonpersistent ABI changed with the x8/x9 experiment");
}

void successful_links_carry_the_fixed_pair_without_memory_traffic() {
  constexpr std::uint64_t pc = 0x5a00'1000;
  constexpr std::array guest{
      0x9100'0508U, // add x8, x8, #1
      0x9100'0929U, // add x9, x9, #2
      0x1400'0001U, // b pc + 4
  };
  const auto compiled = jit_test::compile_linked_block(pc, guest);
  const auto hot = hot_code(compiled);
  const auto branch = std::find(hot.begin(), hot.end(), 0xd61f'01a0U);
  require(branch != hot.end(), "fixed-bank block has no linked BR");
  constexpr std::uint32_t bank_load = 0xa944'5df6U;
  constexpr std::uint32_t bank_store = 0xa904'5df6U;
  if (jit_test::linked_x8_x9_bank_enabled()) {
    require(count_word(hot, 0x9100'06d6U) == 1 &&
                count_word(hot, 0x9100'0af7U) == 1,
            "guest x8/x9 do not use fixed host x22/x23");
    require(count_word(hot, bank_load) == 1,
            "fixed bank is not loaded exactly once at outer entry");
    require(std::find(hot.begin(), branch, bank_store) == branch,
            "successful link publishes the fixed bank");
    require(std::find(branch, hot.end(), bank_store) != hot.end(),
            "link fallback does not publish the fixed bank");
  } else {
    require(count_word(hot, bank_load) == 0 &&
                count_word(hot, bank_store) == 0,
            "control arm contains fixed-bank traffic");
  }
}

void ls64_x8_group_has_explicit_fixed_bank_boundaries() {
  constexpr std::uint64_t pc = 0x5a00'2000;
  constexpr std::uint32_t bank_load = 0xa944'5df6U;
  constexpr std::uint32_t bank_store = 0xa904'5df6U;
  constexpr std::array cases{
      std::array{0xf83f'd0c8U, 0xd400'0001U}, // ld64b x8, [x6]
      std::array{0xf83f'90c8U, 0xd400'0001U}, // st64b x8, [x6]
  };
  constexpr std::array native_words{0xf83f'd100U, 0xf83f'9100U};

  for (std::size_t index = 0; index < cases.size(); ++index) {
    const auto compiled = jit_test::compile_block_with_feature_override(
        pc + index * 0x100U, cases[index],
        ::uml::cpu::ArmFeature::ls64, true);
    const auto hot = hot_code(compiled);
    const auto native = std::find(hot.begin(), hot.end(), native_words[index]);
    require(native != hot.end(), "LS64 did not retain its native operation");

    if (jit_test::linked_x8_x9_bank_enabled()) {
      const auto body = hot.begin() +
                        static_cast<std::ptrdiff_t>(
                            compiled.internal_entry_word);
      require(body <= native,
              "LS64 native operation precedes the internal entry");
      require(std::find(body, native, bank_store) != native,
              "LS64 did not publish x8/x9 before materializing x0-x7");
      require(std::find(native + 1, hot.end(), bank_load) != hot.end(),
              "LS64 did not reload x8/x9 after the constrained sequence");
    } else {
      require(count_word(hot, bank_load) == 0 &&
                  count_word(hot, bank_store) == 0,
              "control LS64 shape contains fixed-bank traffic");
    }
  }
}

void continuing_tlb_helper_publishes_and_reloads_the_fixed_pair() {
  constexpr std::uint64_t pc = 0x5a00'3000;
  constexpr std::array guest{
      0x9100'0508U, // add x8, x8, #1
      0x3940'00c0U, // ldrb w0, [x6]
      0x9100'0929U, // add x9, x9, #2
      0xd400'0001U, // svc #0
  };
  constexpr std::uint32_t bank_load = 0xa944'5df6U;
  constexpr std::uint32_t bank_store = 0xa904'5df6U;
  const auto compiled = jit_test::compile_block(pc, guest);
  const auto all = std::span<const std::uint32_t>{compiled.words};
  require(compiled.hot_words <= all.size(),
          "TLB helper hot boundary exceeds generated code");
  const auto cold = all.subspan(compiled.hot_words);
  require(count_word(cold, 0xd63f'0200U) != 0,
          "memory cold paths contain no host call");

  if (jit_test::linked_x8_x9_bank_enabled()) {
    std::optional<std::size_t> returning_call;
    for (std::size_t index = 0; index + 1U < cold.size(); ++index)
      if (cold[index] == 0xd63f'0200U && cold[index + 1U] == bank_load) {
        require(!returning_call.has_value(),
                "more than one memory helper reloads the fixed bank");
        returning_call = index;
      }
    require(returning_call.has_value(),
            "TLB miss helper did not reload x8/x9 before continuing");
    require(std::find(cold.begin(),
                      cold.begin() + static_cast<std::ptrdiff_t>(
                                         *returning_call),
                      bank_store) !=
                cold.begin() +
                    static_cast<std::ptrdiff_t>(*returning_call),
            "TLB miss helper did not publish x8/x9 before C++");
  } else {
    require(count_word(cold, bank_load) == 0 &&
                count_word(cold, bank_store) == 0,
            "control TLB helper shape contains fixed-bank traffic");
  }
}

void terminal_helper_publishes_the_bank_only_before_calling_cpp() {
  constexpr std::uint64_t pc = 0x5a00'3800;
  constexpr std::array guest{
      0xc85f'7c08U, // ldxr x8, [x0]
  };
  constexpr std::uint32_t bank_load = 0xa944'5df6U;
  constexpr std::uint32_t bank_store = 0xa904'5df6U;
  const auto compiled = jit_test::compile_block(pc, guest);
  const auto all = std::span<const std::uint32_t>{compiled.words};
  const auto call = std::find(all.begin(), all.end(), 0xd63f'0200U);
  require(call != all.end(), "exclusive terminal helper has no C++ call");

  if (jit_test::linked_x8_x9_bank_enabled()) {
    require(std::find(all.begin(), call, bank_store) != call,
            "terminal helper did not publish x8/x9 before C++");
    require(std::find(call + 1, all.end(), bank_store) == all.end(),
            "terminal helper republishes stale x8/x9 after C++");
    require(std::find(call + 1, all.end(), bank_load) == all.end(),
            "terminal helper reloads an unused x8/x9 bank after C++");
  } else {
    require(count_word(all, bank_load) == 0 &&
                count_word(all, bank_store) == 0,
            "control terminal-helper shape contains fixed-bank traffic");
  }
}

void memory_base_x8_uses_the_fixed_bank() {
  constexpr std::uint64_t pc = 0x5a00'4000;
  constexpr std::array guest{
      0x3940'0100U, // ldrb w0, [x8]
      0xd400'0001U, // svc #0
  };
  constexpr std::uint32_t move_base_from_bank = 0xaa16'03e8U;
  constexpr std::uint32_t load_base_from_state = 0xf940'21e8U;
  const auto compiled = jit_test::compile_block(pc, guest);
  const auto hot = hot_code(compiled);

  if (jit_test::linked_x8_x9_bank_enabled()) {
    require(count_word(hot, move_base_from_bank) == 1,
            "guest x8 memory base did not use fixed host x22");
    require(count_word(hot, load_base_from_state) == 0,
            "guest x8 memory base reloaded stale CpuState");
  } else {
    require(count_word(hot, move_base_from_bank) == 0,
            "control memory-base shape uses the fixed bank");
    require(count_word(hot, load_base_from_state) == 1,
            "control memory-base shape stopped loading CpuState");
  }
}

void direct_success_path_has_scheduled_remaining_budget() {
  constexpr std::uint64_t pc = 0x5a01'0000;
  constexpr std::array guest{0x1400'0001U}; // b pc + 4
  const auto resident_code = jit_test::compile_linked_block(pc, guest);
  const auto memory_code =
      jit_test::compile_linked_block_memory_abi(pc, guest);
  const auto resident = hot_code(resident_code);
  const auto memory = hot_code(memory_code);
  const auto resident_literal =
      require_single_literal_pointer(resident_code, 12);
  const auto memory_literal =
      require_single_literal_pointer(memory_code, 12);

  const auto resident_words =
      successful_path_words(resident, resident_literal.source);
  const auto memory_words =
      successful_path_words(memory, memory_literal.source);
  require(memory_words == 12 && resident_words == 8,
          "direct successful path word count changed");
  require(memory_words - resident_words ==
              jit_test::persistent_link_successful_path_word_savings(),
          "direct persistent-path saving is not exactly four words");
  require(resident_literal.source + 2U < resident.size() &&
              resident[resident_literal.source + 1U] == 0xd100'0694U &&
              resident[resident_literal.source + 2U] == 0xf875'798dU,
          "direct path does not schedule SUB behind its literal load");
  require(count_word(resident, 0xeb09'029fU) == 1,
          "direct path does not compare remaining budget with target count");
  const auto selector_fallback = require_unique_instruction(
      resident, 0xff00'001fU, 0xb400'000dU,
      "direct path does not have one selector fallback");
  const auto fairness_fallback = require_unique_instruction(
      resident, 0xff00'001fU, 0x5400'0003U,
      "direct path does not have one unsigned fairness fallback");
  const auto selector_target = require_branch_target(
      resident, selector_fallback, "direct selector fallback is invalid");
  const auto fairness_target = require_branch_target(
      resident, fairness_fallback, "direct fairness fallback is invalid");
  require(selector_target == fairness_target &&
              resident[selector_target] == 0x9100'0694U,
          "direct selector and fairness exits do not share the cold undo");
  require(count_word(memory, 0xd100'054aU) == 1 &&
              count_word(memory, 0xeb09'015fU) == 1 &&
              count_word(memory, 0xf900'016aU) == 1,
          "memory ABI does not subtract, compare, and commit remaining budget");
  require(count_word(resident, 0x9100'068aU) == 0 &&
              count_word(resident, 0x8b09'0148U) == 0 &&
              count_word(resident, 0xf140'051fU) == 0 &&
              count_word(resident, 0xaa0a'03f4U) == 0,
          "direct path retains the accumulated-count sequence");
  require(count_word(resident, 0xf940'13ebU) == 0 &&
              count_word(resident, 0x3940'6169U) == 0 &&
              count_word(resident, 0xf940'016aU) == 0 &&
              count_word(resident, 0xf900'016aU) == 0,
          "direct path still accesses memory-backed chain state");
  require(count_word(resident, 0xf900'0a69U) == 0 &&
              count_word(resident, 0xf900'0670U) == 0 &&
              count_word(memory, 0xf900'0969U) == 0 &&
              count_word(memory, 0xf900'0570U) == 0,
          "direct success still publishes obsolete terminal metadata");
  require_only_native_br_x13(resident, 1);
}

void conditional_arms_each_use_the_persistent_path() {
  constexpr std::uint64_t pc = 0x5a02'0000;
  constexpr std::array guest{0x5400'0040U}; // b.eq pc + 8
  const auto compiled = jit_test::compile_linked_block(pc, guest);
  const auto hot = hot_code(compiled);
  require(count_word(hot, 0xf875'798dU) == 2,
          "conditional branch does not select both successors through x21");
  require(count_word(hot, 0xd100'0694U) == 2 &&
              count_word(hot, 0x9100'0694U) == 2,
          "conditional arms do not subtract and cold-restore their instruction");
  require(count_word(hot, 0xd61f'01a0U) == 2,
          "conditional branch does not have two host-owned successor BRs");
  require(count_word(hot, 0xf900'0a69U) == 0 &&
              count_word(hot, 0xf900'0670U) == 0,
          "conditional success still publishes obsolete terminal metadata");

  std::array<std::size_t, 2> literal_sources{};
  std::array<std::size_t, 2> literal_targets{};
  std::size_t literal_count = 0;
  for (std::size_t source = 0; source < hot.size(); ++source) {
    const auto word = hot[source];
    if ((word & 0xff00'001fU) != (0x5800'0000U | 12U))
      continue;
    require(literal_count < literal_sources.size(),
            "conditional branch exceeds the two-pointer patch bound");
    const auto displacement =
        sign_extend((word >> 5U) & 0x7ffffU, 19);
    require(displacement >= 0,
            "conditional successor literal points before its load");
    literal_sources[literal_count] = source;
    literal_targets[literal_count] =
        source + static_cast<std::size_t>(displacement);
    ++literal_count;
  }
  require(literal_count == literal_sources.size(),
          "conditional branch does not use exactly two stable pointers");
  require(literal_targets[0] >= compiled.hot_words &&
              literal_targets[1] >= compiled.hot_words &&
              (literal_targets[0] & 1U) == 0U &&
              (literal_targets[1] & 1U) == 0U &&
              literal_targets[0] != literal_targets[1] &&
              literal_targets[0] + 1U < compiled.words.size() &&
              literal_targets[1] + 1U < compiled.words.size(),
          "conditional stable pointers are not distinct aligned tail entries");
  const auto literal_value = [&](std::size_t target) {
    return static_cast<std::uint64_t>(compiled.words[target]) |
           (static_cast<std::uint64_t>(compiled.words[target + 1U]) << 32U);
  };
  require(literal_value(literal_targets[0]) != 0 &&
              literal_value(literal_targets[0]) ==
                  literal_value(literal_targets[1]),
          "conditional successor literals do not preserve their host pointer");
  for (const auto word : hot)
    require(!is_move_wide_to(word, 12),
            "conditional hot path materializes a stable pointer");
}

void pic_success_path_has_scheduled_remaining_budget() {
  constexpr std::uint64_t pc = 0x5a03'0000;
  constexpr std::array guest{0xd61f'00a0U}; // br x5
  const auto resident_code =
      jit_test::compile_indirect_linked_block(pc, guest);
  const auto memory_code =
      jit_test::compile_indirect_linked_block_memory_abi(pc, guest);
  const auto resident = hot_code(resident_code);
  const auto memory = hot_code(memory_code);
  const auto resident_literal =
      require_single_literal_pointer(resident_code, 13);
  const auto memory_literal =
      require_single_literal_pointer(memory_code, 13);

  const auto resident_words =
      successful_path_words(resident, resident_literal.source);
  const auto memory_words =
      successful_path_words(memory, memory_literal.source);
  require(memory_words == 18 && resident_words == 14,
          "PIC successful path word count changed");
  require(memory_words - resident_words == 4,
          "PIC persistent-path saving is not exactly four words");
  require(resident_literal.source + 2U < resident.size() &&
              resident[resident_literal.source + 1U] == 0xd100'0694U &&
              resident[resident_literal.source + 2U] == 0xa940'31a9U,
          "PIC path does not schedule SUB behind its site literal load");
  require(count_word(resident, 0xeb09'029fU) == 1,
          "PIC path does not compare remaining budget with target count");
  const auto selector_fallback = require_unique_instruction(
      resident, 0xff00'001fU, 0xb400'000dU,
      "PIC path does not have one selector fallback");
  const auto fairness_fallback = require_unique_instruction(
      resident, 0xff00'001fU, 0x5400'0003U,
      "PIC path does not have one unsigned fairness fallback");
  require(require_branch_target(resident, selector_fallback,
                                "PIC selector fallback is invalid") ==
              require_branch_target(resident, fairness_fallback,
                                    "PIC fairness fallback is invalid"),
          "PIC selector and fairness exits do not share their fallback");
  const auto undo = require_unique_instruction(
      resident, 0xffff'ffffU, 0x9100'0694U,
      "PIC path does not have exactly one cold budget undo");
  const auto miss_jump = require_unique_instruction(
      resident, 0x7c00'0000U, 0x1400'0000U,
      "PIC miss does not have one branch to the shared undo");
  require(require_branch_target(resident, miss_jump,
                                "PIC miss undo branch is invalid") == undo,
          "PIC miss does not rejoin at the shared cold undo");
  const auto fallback_target = require_branch_target(
      resident, selector_fallback, "PIC fallback target is invalid");
  require(fallback_target + 1U == undo,
          "PIC matched fallback does not fall through to the shared undo");
  require(count_word(memory, 0xd100'054aU) == 1 &&
              count_word(memory, 0xeb09'015fU) == 1 &&
              count_word(memory, 0xf900'016aU) == 1 &&
              count_word(memory, 0x9100'0694U) == 0,
          "PIC memory ABI remaining-budget sequence changed");
  require(count_word(resident, 0xf900'0a69U) == 0 &&
              count_word(resident, 0xf900'0670U) == 0 &&
              count_word(memory, 0xf900'0969U) == 0 &&
              count_word(memory, 0xf900'0570U) == 0,
          "PIC success still publishes obsolete terminal metadata");
  for (const auto word : resident)
    if ((word & 0xffff'fc1fU) == 0xd61f'0000U)
      require(((word >> 5U) & 31U) == 13U,
              "PIC native branch consumes a guest-controlled register");
  require_only_native_br_x13(resident, 1);
}

} // namespace

int main() {
  out_of_range_literal_uses_cold_absolute_fallback();
  two_pointer_literals_use_the_fixed_capacity_pool();
  two_out_of_range_literals_use_independent_cold_fallbacks();
  frame_shape_saves_and_restores_host_chain_registers();
  successful_links_carry_the_fixed_pair_without_memory_traffic();
  ls64_x8_group_has_explicit_fixed_bank_boundaries();
  continuing_tlb_helper_publishes_and_reloads_the_fixed_pair();
  terminal_helper_publishes_the_bank_only_before_calling_cpp();
  memory_base_x8_uses_the_fixed_bank();
  direct_success_path_has_scheduled_remaining_budget();
  conditional_arms_each_use_the_persistent_path();
  pic_success_path_has_scheduled_remaining_budget();
  return 0;
}
