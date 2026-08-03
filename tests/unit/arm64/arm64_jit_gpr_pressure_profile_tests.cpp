// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/jit/jit_test_api.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>

#if !defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
#error "GPR-pressure tests require DARWIN_UML_JIT_GPR_PRESSURE_PROFILE"
#endif

namespace jit = uml::cpu::arm64::jit;
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

[[nodiscard]] constexpr std::uint32_t
add_register(unsigned destination, unsigned left,
             unsigned right) noexcept {
  return 0x8b00'0000U | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
load_post_index(unsigned destination, unsigned base,
                unsigned byte_displacement) noexcept {
  return 0xf840'0400U |
         ((byte_displacement & 0x1ffU) << 12U) |
         ((base & 31U) << 5U) | (destination & 31U);
}

constexpr std::array pressure_guest{
    // Four two-register operations fill the real eight-slot bank.
    add_register(0, 1, 1),
    add_register(2, 3, 3),
    add_register(4, 5, 5),
    add_register(6, 7, 7),

    // Each overflow pair is touched twice. A hypothetical resident slot
    // removes one repeated source load or one intermediate destination store.
    add_register(8, 9, 9),
    add_register(8, 9, 9),
    add_register(10, 11, 11),
    add_register(10, 11, 11),
    add_register(12, 13, 13),
    add_register(12, 13, 13),
    add_register(14, 15, 15),
    add_register(14, 15, 15),
};

void no_overflow_reports_only_slot_high_water() {
  constexpr std::array guest{
      add_register(0, 1, 1),
      add_register(0, 0, 1),
  };
  const auto compiled =
      jit_test::compile_gpr_pressure_profile_block(
          0x5c00'0000, guest, true);
  const auto &profile = compiled.gpr_pressure;
  require(profile.slot_high_water == 2,
          "low-pressure block has the wrong slot high-water mark");
  require(profile.allocation_refusals == 0,
          "low-pressure block reported an allocation refusal");
  require(profile.fallback_read_loads == 0,
          "low-pressure block reported an overflow read load");
  require(profile.fallback_write_stores == 0,
          "low-pressure block reported an overflow write store");
  require(profile.repeated_overflow_accesses == 0,
          "low-pressure block reported a repeated overflow access");
  require(profile.estimated_avoided_accesses ==
              std::array<std::size_t, 3>{0, 0, 0},
          "low-pressure block predicted capacity savings");
}

void deliberately_disabled_cache_is_not_register_pressure() {
  constexpr std::array guest{
      add_register(0, 1, 1),
      add_register(2, 3, 3),
  };
  const auto compiled =
      jit_test::compile_tracked_block(0x5c00'0800, guest);
  require(compiled.gpr_pressure == jit::JitGprPressureProfile{},
          "tracked translation's disabled cache was reported as pressure");
}

void overflow_counters_and_capacity_estimates_are_exact() {
  static_assert(jit::jit_gpr_pressure_candidate_capacities ==
                std::array<std::size_t, 3>{10, 12, 15});
  const auto compiled =
      jit_test::compile_gpr_pressure_profile_block(
          0x5c00'1000, pressure_guest, true);
  require(compiled.guest_instructions == pressure_guest.size(),
          "pressure block did not compile completely");
  const auto &profile = compiled.gpr_pressure;
  require(profile.slot_high_water == 8,
          "pressure block did not fill the resident bank");
  require(profile.allocation_refusals == 16,
          "native allocation refusals were not counted exactly");
  require(profile.fallback_read_loads == 8,
          "overflow fallback loads were not counted exactly");
  require(profile.fallback_write_stores == 8,
          "overflow fallback stores were not counted exactly");
  require(profile.repeated_overflow_accesses == 8,
          "repeated overflow identities were not counted exactly");
  require(profile.estimated_avoided_accesses ==
              std::array<std::size_t, 3>{2, 4, 7},
          "capacity 10/12/15 avoided-access estimates are wrong");
}

void saturated_nonallocating_accesses_join_the_shadow_bank() {
  constexpr std::array guest{
      add_register(0, 1, 1),
      add_register(2, 3, 3),
      add_register(4, 5, 5),
      add_register(6, 7, 7),
      // The post-index path reads X20 twice through load_gpr() and writes it
      // once through store_gpr(). It does not call allocate_gpr_slot().
      load_post_index(0, 20, 8),
  };
  const auto compiled =
      jit_test::compile_gpr_pressure_profile_block(
          0x5c00'1800, guest, true);
  require(compiled.guest_instructions == guest.size(),
          "post-index pressure block did not compile completely");
  const auto &profile = compiled.gpr_pressure;
  require(profile.slot_high_water == 8,
          "nonallocating pressure block did not saturate the bank");
  require(profile.allocation_refusals == 0,
          "nonallocating pressure access was called an allocation refusal");
  require(profile.fallback_read_loads == 2,
          "nonallocating fallback reads were not counted");
  require(profile.fallback_write_stores == 1,
          "nonallocating fallback write was not counted");
  require(profile.repeated_overflow_accesses == 2,
          "nonallocating repeated accesses were not counted");
  require(profile.estimated_avoided_accesses ==
              std::array<std::size_t, 3>{1, 1, 1},
          "nonallocating shadow-capacity estimate is wrong");
}

void instrumentation_does_not_change_generated_code() {
  constexpr std::uint64_t pc = 0x5c00'2000;
  const auto disabled =
      jit_test::compile_gpr_pressure_profile_block(
          pc, pressure_guest, false);
  const auto enabled =
      jit_test::compile_gpr_pressure_profile_block(
          pc, pressure_guest, true);
  require(disabled.gpr_pressure == jit::JitGprPressureProfile{},
          "runtime-disabled diagnostic counters are not zero");
  require(enabled.gpr_pressure.allocation_refusals != 0,
          "enabled diagnostic did not observe register pressure");
  require(enabled.words == disabled.words,
          "translation-only instrumentation changed generated words");
  require(enabled.hot_words == disabled.hot_words,
          "translation-only instrumentation changed hot-code size");
  require(enabled.guest_instructions == disabled.guest_instructions,
          "translation-only instrumentation changed block length");
  require(enabled.internal_entry_word == disabled.internal_entry_word,
          "translation-only instrumentation changed the internal entry");
  require(enabled.memory_write_mask == disabled.memory_write_mask &&
              enabled.direct_memory_write_mask ==
                  disabled.direct_memory_write_mask,
          "translation-only instrumentation changed memory metadata");
  require(enabled.accesses_fp_state == disabled.accesses_fp_state &&
              enabled.link_target_safe == disabled.link_target_safe &&
              enabled.native_fp == disabled.native_fp &&
              enabled.native_fpmr == disabled.native_fpmr &&
              enabled.guest_illegal == disabled.guest_illegal,
          "translation-only instrumentation changed block metadata");
}

} // namespace

int main() {
  no_overflow_reports_only_slot_high_water();
  deliberately_disabled_cache_is_not_register_pressure();
  overflow_counters_and_capacity_estimates_are_exact();
  saturated_nonallocating_accesses_join_the_shadow_bank();
  instrumentation_does_not_change_generated_code();
  return 0;
}
