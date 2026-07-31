// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/cpu_state.hpp"
#include "runtime/jit/jit_test_api.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <span>
#include <vector>

namespace arm64 = uml::backend::arm64;
namespace jit_test = uml::backend::jit::testing;

namespace {

constexpr std::uint64_t code_base = 0x5040'0000ULL;
constexpr std::uint32_t memory_register_mask = (31U << 5U) | 31U;
// One flag-neutral first-way tag comparison is emitted per TLB translation.
constexpr std::uint32_t translation_probe = 0xca01'0042U;
constexpr std::uint32_t page_span_eor = 0xca08'0000U; // eor x0, x0, x8
constexpr unsigned page_shift =
    DARWIN_UML_GUEST_PAGE_SIZE == 16384 ? 14U : 12U;
constexpr std::uint32_t page_span_tbnz =
    0x3700'0000U | ((page_shift & 31U) << 19U);
constexpr std::uint32_t sp_alignment_test =
    0xf240'001fU | (3U << 10U) | (8U << 5U);
constexpr std::uint32_t monitor_clear =
    0x3900'0000U |
    (static_cast<std::uint32_t>(
         offsetof(arm64::CpuState, exclusive_size))
     << 10U) |
    (15U << 5U) | 31U;
constexpr std::uint32_t nop = 0xd503'201fU;

struct Variant final {
  unsigned size_shift;
  unsigned opc;
  bool load;
};

constexpr std::array variants{
    Variant{0, 0, false}, Variant{0, 1, true},
    Variant{0, 2, true},  Variant{0, 3, true},
    Variant{1, 0, false}, Variant{1, 1, true},
    Variant{1, 2, true},  Variant{1, 3, true},
    Variant{2, 0, false}, Variant{2, 1, true},
    Variant{2, 2, true},  Variant{3, 0, false},
    Variant{3, 1, true},
};

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

void require_count(std::size_t actual, std::size_t expected,
                   const char *message,
                   const std::source_location location =
                       std::source_location::current()) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "%s: expected=%zu actual=%zu at %s:%u\n", message,
               expected, actual, location.file_name(), location.line());
  std::abort();
}

[[nodiscard]] constexpr std::uint32_t
unsigned_memory(const Variant &variant, unsigned immediate, unsigned base,
                unsigned transfer) noexcept {
  return 0x3900'0000U | (variant.size_shift << 30U) |
         (variant.opc << 22U) | ((immediate & 0xfffU) << 10U) |
         ((base & 31U) << 5U) | (transfer & 31U);
}

[[nodiscard]] constexpr std::uint32_t
unsigned_x(bool load, unsigned immediate, unsigned base,
           unsigned transfer) noexcept {
  return unsigned_memory(Variant{3, load ? 1U : 0U, load}, immediate, base,
                         transfer);
}

[[nodiscard]] std::vector<std::uint32_t>
hot_code(const jit_test::CompiledCode &compiled, std::size_t expected_guest) {
  require(compiled.guest_instructions == expected_guest,
          "JIT did not compile the complete GPR-memory program");
  require(compiled.hot_words <= compiled.words.size(),
          "JIT hot-word boundary exceeds generated code");
  return {compiled.words.begin(),
          compiled.words.begin() +
              static_cast<std::ptrdiff_t>(compiled.hot_words)};
}

[[nodiscard]] std::size_t count_word(std::span<const std::uint32_t> words,
                                     std::uint32_t word) {
  std::size_t count = 0;
  for (const auto current : words)
    count += current == word;
  return count;
}

[[nodiscard]] std::size_t
count_page_span_tbnz(std::span<const std::uint32_t> words) {
  constexpr std::uint32_t branch_displacement_mask = 0x0007'ffe0U;
  std::size_t count = 0;
  for (const auto current : words)
    count += (current & ~branch_displacement_mask) == page_span_tbnz;
  return count;
}

[[nodiscard]] std::size_t count_memory_form(
    std::span<const std::uint32_t> words, std::uint32_t guest) {
  std::size_t count = 0;
  for (const auto current : words) {
    if ((current & ~memory_register_mask) !=
        (guest & ~memory_register_mask))
      continue;
    /*
     * TLB scaffolding can contain an unrelated load with the same size,
     * opcode, and immediate. Count only the translated guest form, whose
     * construction fixes Rn to the checked host address in x8.
     */
    if (((current >> 5U) & 31U) != 8U)
      continue;
    const auto transfer = current & 31U;
    const bool zero_register = (guest & 31U) == 31U;
    const bool safe_transfer =
        transfer == 3U || transfer == 4U || transfer == 5U ||
        transfer == 6U || transfer == 7U || transfer == 9U ||
        transfer == 12U || transfer == 13U || transfer == 17U ||
        (zero_register && transfer == 31U);
    require(safe_transfer,
            "coalesced GPR memory operation used an executor-owned register");
    ++count;
  }
  return count;
}

void every_scalar_variant_coalesces() {
  for (std::size_t index = 0; index < variants.size(); ++index) {
    const auto &variant = variants[index];
    const std::array guest{
        unsigned_memory(variant, 1, 2, 3),
        unsigned_memory(variant, 3, 2, 4),
    };
    const auto compiled =
        jit_test::compile_block(code_base + index * 0x100U, guest);
    const auto hot = hot_code(compiled, guest.size());
    require(count_word(hot, translation_probe) == 1,
            "allocated scalar GPR pair did not share one TLB translation");
    require(count_memory_form(hot, guest[0]) == 1,
            "first scalar GPR operation lost its native offset");
    require(count_memory_form(hot, guest[1]) == 1,
            "second scalar GPR operation lost its native offset");
    require(count_word(hot, sp_alignment_test) == 0,
            "non-SP memory run emitted an SP alignment check");
    const auto expected_mask =
        variant.load ? std::uint64_t{0} : std::uint64_t{3};
    require(compiled.memory_write_mask == expected_mask,
            "GPR pair has the wrong memory-write mask");
    require(compiled.direct_memory_write_mask == expected_mask,
            "GPR pair has the wrong direct-store mask");
    require(count_word(hot, monitor_clear) == (variant.load ? 0U : 1U),
            "GPR store pair did not use one local-monitor clear");

    if (!variant.load) {
      const auto tracked = jit_test::compile_tracked_block(
          code_base + 0x2000U + index * 0x100U, guest);
      const auto tracked_hot = hot_code(tracked, guest.size());
      require(count_word(tracked_hot, translation_probe) == guest.size(),
              "tracked-store GPR translation unexpectedly coalesced");
      require(tracked.memory_write_mask == compiled.memory_write_mask &&
                  tracked.direct_memory_write_mask ==
                      compiled.direct_memory_write_mask,
              "tracked and ordinary GPR store masks differ");
    }
  }
}

void run_lengths_are_bounded_to_four() {
  for (std::size_t length = 2; length <= 6; ++length) {
    std::vector<std::uint32_t> guest;
    guest.reserve(length);
    for (std::size_t index = 0; index < length; ++index)
      guest.push_back(unsigned_x(
          true, static_cast<unsigned>(index), 2,
          static_cast<unsigned>(index + 3U)));
    const auto compiled = jit_test::compile_block(
        code_base + 0x4000U + length * 0x100U, guest);
    const auto hot = hot_code(compiled, guest.size());
    require(count_word(hot, translation_probe) == (length + 3U) / 4U,
            "GPR memory run did not split at four instructions");
  }
}

void grouping_boundaries_are_exact() {
  const std::array mixed_direction{
      unsigned_x(false, 0, 2, 3),
      unsigned_x(true, 1, 2, 4),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x5000U,
                                               mixed_direction),
                       mixed_direction.size()),
              translation_probe) == mixed_direction.size(),
          "mixed GPR load/store direction was coalesced");

  const std::array mixed_base{
      unsigned_x(true, 0, 2, 3),
      unsigned_x(true, 1, 5, 4),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x5100U,
                                               mixed_base),
                       mixed_base.size()),
              translation_probe) == mixed_base.size(),
          "different GPR base registers were coalesced");

  const std::array mixed_sp{
      unsigned_x(true, 0, 2, 3),
      unsigned_x(true, 1, 31, 4),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x5200U,
                                               mixed_sp),
                       mixed_sp.size()),
              translation_probe) == mixed_sp.size(),
          "GPR and SP operations were coalesced together");
}

void base_alias_rules_are_exact() {
  const std::array first_load_alias{
      unsigned_x(true, 0, 2, 2),
      unsigned_x(true, 1, 2, 3),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x5800U,
                                               first_load_alias),
                       first_load_alias.size()),
              translation_probe) == first_load_alias.size(),
          "load Rt==Rn was accepted as the first operation of a run");

  const std::array second_load_alias{
      unsigned_x(true, 0, 2, 3),
      unsigned_x(true, 1, 2, 2),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x5900U,
                                               second_load_alias),
                       second_load_alias.size()),
              translation_probe) == second_load_alias.size(),
          "load Rt==Rn was accepted later in a run");

  const std::array alias_after_safe_prefix{
      unsigned_x(true, 0, 2, 3),
      unsigned_x(true, 1, 2, 4),
      unsigned_x(true, 2, 2, 2),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x5a00U,
                                               alias_after_safe_prefix),
                       alias_after_safe_prefix.size()),
              translation_probe) == 2,
          "load Rt==Rn did not terminate the preceding safe run");

  const std::array store_alias{
      unsigned_x(false, 0, 2, 2),
      unsigned_x(false, 1, 2, 3),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x5b00U,
                                               store_alias),
                       store_alias.size()),
              translation_probe) == 1,
          "safe store Rt==Rn pair did not coalesce");
}

void page_limits_select_only_safe_prefixes() {
  constexpr unsigned page_units = DARWIN_UML_GUEST_PAGE_SIZE / 8U;
  const auto near = unsigned_x(true, 0, 2, 3);
  const auto exact = unsigned_x(true, page_units - 1U, 2, 4);
  const auto beyond = unsigned_x(true, page_units, 2, 5);

  const std::array exact_guest{near, exact};
  const auto exact_compiled =
      jit_test::compile_block(code_base + 0x6000U, exact_guest);
  const auto exact_hot = hot_code(exact_compiled, exact_guest.size());
  require(count_word(exact_hot, translation_probe) == 1,
          "exact-page-span GPR pair was not coalesced");
  require(count_word(exact_hot, page_span_eor) == 1,
          "exact-page-span GPR pair omitted its end-address comparison");
  require(count_page_span_tbnz(exact_hot) == 1,
          "exact-page-span GPR pair omitted its page-bit rejection");

  const std::array rejected{beyond, beyond};
  const auto rejected_compiled =
      jit_test::compile_block(code_base + 0x6100U, rejected);
  require(count_word(hot_code(rejected_compiled, rejected.size()),
                     translation_probe) ==
              rejected.size(),
          "out-of-page static GPR offsets were coalesced");

  const std::array shrunk{near, near, beyond};
  const auto shrunk_compiled =
      jit_test::compile_block(code_base + 0x6200U, shrunk);
  require(count_word(hot_code(shrunk_compiled, shrunk.size()),
                     translation_probe) == 2,
          "unsafe GPR tail did not shrink to a safe two-instruction prefix");

  const std::array same_byte{
      unsigned_memory(variants[1], 0, 2, 3),
      unsigned_memory(variants[1], 0, 2, 4),
  };
  const auto same_compiled =
      jit_test::compile_block(code_base + 0x6300U, same_byte);
  const auto same_hot = hot_code(same_compiled, same_byte.size());
  require(count_word(same_hot, translation_probe) == 1,
          "same-byte GPR loads did not coalesce");
  require(count_word(same_hot, page_span_eor) == 0 &&
              count_page_span_tbnz(same_hot) == 0,
          "zero-width page span emitted a redundant runtime proof");
}

void infrastructure_guest_registers_are_remapped() {
  for (unsigned base = 0; base < 31U; ++base) {
    const unsigned first = (base + 1U) % 31U;
    const unsigned second = (base + 2U) % 31U;
    const std::array guest{
        unsigned_x(true, 4, base, first),
        unsigned_x(true, 5, base, second),
    };
    const auto compiled = jit_test::compile_block(
        code_base + 0x6800U + base * 0x80U, guest);
    const auto hot = hot_code(compiled, guest.size());
    require(count_word(hot, translation_probe) == 1,
            "infrastructure guest base pair did not coalesce");
    require(count_memory_form(hot, guest[0]) == 1 &&
                count_memory_form(hot, guest[1]) == 1,
            "infrastructure guest base was not remapped to x8");
  }

  for (unsigned transfer = 0; transfer < 32U; ++transfer) {
    for (const bool load : {false, true}) {
      const unsigned base = transfer == 2U ? 3U : 2U;
      const std::array guest{
          unsigned_x(load, 20, base, transfer),
          unsigned_x(load, 21, base, transfer),
      };
      const auto compiled = jit_test::compile_block(
          code_base + 0x8000U + transfer * 0x100U +
              (load ? 0x80U : 0U),
          guest);
      const auto hot = hot_code(compiled, guest.size());
      require(count_word(hot, translation_probe) == 1,
              "infrastructure transfer-register pair did not coalesce");
      require(count_memory_form(hot, guest[0]) == 1 &&
                  count_memory_form(hot, guest[1]) == 1,
              "infrastructure transfer register was not remapped");
    }
  }
}

void store_masks_include_every_grouped_instruction() {
  const std::array guest{
      nop,
      unsigned_x(false, 0, 2, 3),
      unsigned_x(false, 1, 2, 4),
      unsigned_x(false, 2, 2, 5),
      unsigned_x(false, 3, 2, 6),
  };
  const auto compiled = jit_test::compile_block(code_base + 0x8000U, guest);
  const auto hot = hot_code(compiled, guest.size());
  require(compiled.memory_write_mask == 0x1eU,
          "prefixed GPR run omitted a memory-write bit");
  require(compiled.direct_memory_write_mask == 0x1eU,
          "prefixed GPR run omitted a direct-store bit");
  require(count_word(hot, monitor_clear) == 1,
          "four-store GPR run repeated its monitor clear");
}

void reserved_and_unrelated_allocations_do_not_coalesce() {
  constexpr std::array reserved{
      Variant{2, 3, true},
      Variant{3, 3, true},
  };
  for (std::size_t index = 0; index < reserved.size(); ++index) {
    const std::array guest{
        unsigned_memory(reserved[index], 0, 2, 3),
        unsigned_memory(reserved[index], 1, 2, 4),
    };
    const auto compiled =
        jit_test::compile_block(code_base + 0x8800U + index * 0x100U, guest);
    require(compiled.guest_instructions == 1,
            "reserved scalar cell was accepted as a GPR memory run");
    const auto hot = std::span<const std::uint32_t>{compiled.words}.first(
        compiled.hot_words);
    require(count_word(hot, translation_probe) == 0,
            "reserved scalar cell performed a TLB translation");
  }

  const std::array vector{
      std::uint32_t{0x3dc0'0041U},
      std::uint32_t{0x3dc0'0442U},
  };
  const auto vector_hot =
      hot_code(jit_test::compile_block(code_base + 0x8a00U, vector),
               vector.size());
  require_count(
      count_word(vector_hot, translation_probe), 1,
      "vector unsigned memory did not use its dedicated shared run");
  require(count_word(vector_hot, sp_alignment_test) == 0,
          "GPR-based vector run emitted the scalar SP alignment guard");

  const std::array unscaled{
      std::uint32_t{0xf840'0041U},
      std::uint32_t{0xf840'8042U},
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x8b00U, unscaled),
                       unscaled.size()),
              translation_probe) == unscaled.size(),
          "non-unsigned addressing mode was accepted as a GPR run");

  const std::array prefetch{
      std::uint32_t{0xf980'0040U},
      std::uint32_t{0xf980'0441U},
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x8c00U, prefetch),
                       prefetch.size()),
              translation_probe) == 0,
          "prefetch hints performed a memory translation");
}

} // namespace

int main() {
  every_scalar_variant_coalesces();
  run_lengths_are_bounded_to_four();
  grouping_boundaries_are_exact();
  base_alias_rules_are_exact();
  page_limits_select_only_safe_prefixes();
  infrastructure_guest_registers_are_remapped();
  store_masks_include_every_grouped_instruction();
  reserved_and_unrelated_allocations_do_not_coalesce();
  return 0;
}
