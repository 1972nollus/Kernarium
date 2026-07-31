// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/jit/jit_test_api.hpp"
#include "runtime/host_features.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace jit_test = uml::backend::jit::testing;

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

[[nodiscard]] constexpr std::uint32_t bit(unsigned reg) noexcept {
  return std::uint32_t{1} << reg;
}

[[nodiscard]] constexpr std::uint32_t
add_register(unsigned destination, unsigned left, unsigned right,
             bool is_64_bit = true) noexcept {
  return (is_64_bit ? 0x8b00'0000U : 0x0b00'0000U) |
         ((right & 31U) << 16U) | ((left & 31U) << 5U) |
         (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
branch(std::uint64_t source, std::uint64_t target,
       bool writes_link = false) noexcept {
  const auto displacement = (static_cast<std::int64_t>(target) -
                             static_cast<std::int64_t>(source)) /
                            4;
  return (writes_link ? 0x9400'0000U : 0x1400'0000U) |
         (static_cast<std::uint32_t>(displacement) & 0x03ff'ffffU);
}

[[nodiscard]] constexpr std::uint32_t
conditional_branch(unsigned condition, std::uint64_t source,
                   std::uint64_t target) noexcept {
  const auto displacement = (static_cast<std::int64_t>(target) -
                             static_cast<std::int64_t>(source)) /
                            4;
  return 0x5400'0000U |
         ((static_cast<std::uint32_t>(displacement) & 0x7ffffU) << 5U) |
         (condition & 15U);
}

[[nodiscard]] constexpr std::uint32_t
branch_register(unsigned source) noexcept {
  return 0xd61f'0000U | ((source & 31U) << 5U);
}

void exact_read_write_and_dirty_masks() {
  constexpr std::uint64_t pc = 0x5100'0000;
  constexpr std::array guest{
      add_register(5, 6, 7),
      branch(pc + 4U, pc + 0x100U),
  };
  const auto compiled =
      jit_test::compile_gpr_profile_linked_block(pc, guest, true);
  require(compiled.guest_instructions == guest.size(),
          "profiled arithmetic/link block did not compile");
  require(compiled.gpr_read_before_write_mask == (bit(6) | bit(7)),
          "read-before-write mask is not exact");
  require(compiled.gpr_written_mask == bit(5),
          "written mask is not exact");
  require(compiled.gpr_link_dirty_live_out_mask == bit(5),
          "dirty live-out mask is not the resident dirty set");
}

void rmw_and_w_aliases_share_guest_identity() {
  constexpr std::uint64_t pc = 0x5100'1000;
  constexpr std::array guest{
      add_register(5, 5, 7, false),
      branch(pc + 4U, pc + 0x100U),
  };
  const auto compiled =
      jit_test::compile_gpr_profile_linked_block(pc, guest, true);
  require(compiled.gpr_read_before_write_mask == (bit(5) | bit(7)),
          "W-register RMW inputs were not tracked as X-register identities");
  require(compiled.gpr_written_mask == bit(5),
          "W-register write did not map to the guest X register");
  require(compiled.gpr_link_dirty_live_out_mask == bit(5),
          "W-register dirty live-out did not map to the guest X register");
}

void sp_is_distinct_and_zero_register_is_excluded() {
  constexpr std::uint64_t pc = 0x5100'2000;
  constexpr std::array guest{
      add_register(0, 31, 1), // add x0, xzr, x1
      0x9100'07ffU,           // add sp, sp, #1
      branch(pc + 8U, pc + 0x100U),
  };
  const auto compiled =
      jit_test::compile_gpr_profile_linked_block(pc, guest, true);
  require(compiled.gpr_read_before_write_mask == (bit(1) | bit(31)),
          "SP was not tracked distinctly or XZR leaked into live-ins");
  require(compiled.gpr_written_mask == (bit(0) | bit(31)),
          "SP was not tracked distinctly or XZR leaked into writes");
  require(compiled.gpr_link_dirty_live_out_mask == (bit(0) | bit(31)),
          "resident SP write was not tracked as dirty live-out");
}

void bl_marks_x30_dirty() {
  constexpr std::uint64_t pc = 0x5100'3000;
  constexpr std::array guest{
      add_register(5, 6, 7),
      branch(pc + 4U, pc + 0x100U, true),
  };
  const auto compiled =
      jit_test::compile_gpr_profile_linked_block(pc, guest, true);
  require(compiled.gpr_written_mask == (bit(5) | bit(30)),
          "BL did not record its architectural X30 write");
  require(compiled.gpr_link_dirty_live_out_mask == bit(5),
          "BL's already-committed X30 was misclassified as resident dirty");

  constexpr std::array resident_x30{
      add_register(30, 6, 7),
      branch(pc + 4U, pc + 0x100U, true),
  };
  const auto resident =
      jit_test::compile_gpr_profile_linked_block(pc, resident_x30, true);
  require(resident.gpr_link_dirty_live_out_mask == bit(30),
          "BL did not retain an already-resident X30 as dirty live-out");
}

void ls64_direct_state_writes_are_not_dirty_residents() {
  if (!uml::backend::host_arm_features().has(
          uml::backend::ArmFeature::ls64))
    return;
  constexpr std::uint64_t pc = 0x5100'4000;
  constexpr std::array guest{
      add_register(5, 6, 7),
      0xf83f'd0c8U, // ld64b x8, [x6]
      branch(pc + 8U, pc + 0x100U),
  };
  const auto compiled =
      jit_test::compile_gpr_profile_linked_block(pc, guest, true);
  std::uint32_t loaded{};
  for (unsigned reg = 8; reg < 16; ++reg)
    loaded |= bit(reg);
  require(compiled.gpr_read_before_write_mask == (bit(6) | bit(7)),
          "LS64 base/source reads are not exact");
  require(compiled.gpr_written_mask == (bit(5) | loaded),
          "LS64 implicit X8-X15 writes were not tracked");
  require(compiled.gpr_link_dirty_live_out_mask == 0,
          "flushed/direct LS64 state was misclassified as resident dirty");
}

void disabled_resolver_preserves_link_code_shape() {
  constexpr std::uint64_t pc = 0x5100'5000;
  constexpr std::array guest{
      add_register(5, 6, 7),
      branch(pc + 4U, pc + 0x100U),
  };
  require(jit_test::gpr_profile_disabled_resolver_preserves_linked_code(
              pc, guest),
          "diagnostic build changed same-context uninstrumented link code");

  const auto disabled =
      jit_test::compile_gpr_profile_linked_block(pc, guest, false);
  const auto enabled =
      jit_test::compile_gpr_profile_linked_block(pc, guest, true);
  require(enabled.hot_words == disabled.hot_words + 4U,
          "direct counter is not the minimal pointer/load/add/store sequence");
  std::size_t counter_pointer_loads{};
  for (std::size_t index = 0; index < enabled.hot_words; ++index)
    counter_pointer_loads +=
        (enabled.words[index] & 0xff00'001fU) == 0x5800'0009U;
  require(counter_pointer_loads == 1,
          "direct counter pointer is not one literal load");
}

void conditional_profile_uses_bounded_literal_metadata() {
  constexpr std::uint64_t pc = 0x5100'5800;
  constexpr std::array guest{
      conditional_branch(0, pc, pc + 0x100U),
  };
  const auto compiled =
      jit_test::compile_gpr_profile_linked_block(pc, guest, true);
  require(compiled.guest_instructions == guest.size(),
          "profiled conditional branch did not compile");
  std::size_t metadata_literals{};
  std::size_t guest_pc_literals{};
  for (std::size_t index = 0; index < compiled.hot_words; ++index) {
    const auto word = compiled.words[index];
    if ((word & 0xff00'0000U) != 0x5800'0000U)
      continue;
    const unsigned destination = word & 31U;
    metadata_literals += destination == 9U || destination == 12U;
    guest_pc_literals += destination == 8U;
  }
  require(metadata_literals == 4U && guest_pc_literals == 2U,
          "two-successor profile has the wrong fixed literal shape");
}

void disabled_resolver_preserves_pic_code_shape() {
  constexpr std::uint64_t pc = 0x5100'6000;
  constexpr std::array guest{
      add_register(5, 6, 7),
      branch_register(9),
  };
  require(jit_test::gpr_profile_disabled_resolver_preserves_indirect_code(
              pc, guest),
          "diagnostic build changed same-context uninstrumented PIC code");
}

} // namespace

int main() {
  exact_read_write_and_dirty_masks();
  rmw_and_w_aliases_share_guest_identity();
  sp_is_distinct_and_zero_register_is_excluded();
  bl_marks_x30_dirty();
  ls64_direct_state_writes_are_not_dirty_residents();
  disabled_resolver_preserves_link_code_shape();
  conditional_profile_uses_bounded_literal_metadata();
  disabled_resolver_preserves_pic_code_shape();
  return 0;
}
