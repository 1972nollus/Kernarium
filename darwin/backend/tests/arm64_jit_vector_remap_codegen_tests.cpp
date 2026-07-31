// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/jit/jit_test_api.hpp"
#include "runtime/host_features.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <source_location>
#include <span>
#include <string_view>

namespace jit_test = uml::backend::jit::testing;

namespace {

constexpr std::uint32_t register_mask =
    (31U << 16U) | (31U << 5U) | 31U;
constexpr std::uint32_t constrained_register_mask =
    (15U << 16U) | (31U << 5U) | 31U;
constexpr std::uint32_t two_register_mask = (31U << 5U) | 31U;
constexpr unsigned first_cached_vector = 16U;
constexpr unsigned last_cached_vector = 31U;
constexpr std::uint64_t code_base = 0x4f10'0000ULL;

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
bitwise(unsigned operation, unsigned destination, unsigned left,
        unsigned right) noexcept {
  return 0x4e20'1c00U | ((operation & 3U) << 22U) |
         ((operation & 4U) << 27U) | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
polynomial_multiply(unsigned destination, unsigned left,
                    unsigned right) noexcept {
  return 0x0e20'e000U | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
modified_immediate(bool combine, bool clear, unsigned immediate,
                   unsigned destination) noexcept {
  const unsigned mode = combine ? 1U : 14U;
  return 0x4f00'0400U | (clear ? 0x2000'0000U : 0U) |
         ((immediate & 0xe0U) << 11U) | (mode << 12U) |
         ((immediate & 0x1fU) << 5U) | (destination & 31U);
}

/*
 * The six unrestricted operations bind V0-V15. The final definitions bind
 * V16/V17, filling q6/q7 after q16-q31. If a following constrained operation
 * prebinds V15 in q6, V16 fills the released high slot and V17 fills q7, so
 * all eighteen slots are still occupied before the checked fallback.
 */
constexpr std::array<std::uint32_t, 8> full_vector_cache_fillers{
    bitwise(4, 0, 1, 2),    bitwise(4, 3, 4, 5),
    bitwise(4, 6, 7, 8),    bitwise(4, 9, 10, 11),
    bitwise(4, 12, 13, 14), bitwise(4, 15, 0, 1),
    modified_immediate(false, false, 0x51U, 16U),
    modified_immediate(false, false, 0x52U, 17U),
};

[[nodiscard]] constexpr std::uint32_t
vector_unsigned_memory(bool load, unsigned base, unsigned transfer) noexcept {
  return 0x3d00'0000U | ((load ? 3U : 2U) << 22U) |
         ((base & 31U) << 5U) | (transfer & 31U);
}

[[nodiscard]] constexpr std::uint32_t
atomic_floating(bool store_only, unsigned base, unsigned source,
                unsigned result) noexcept {
  /* Binary32 FADD, relaxed ordering. */
  return 0xbc20'0000U | ((source & 31U) << 16U) |
         (static_cast<unsigned>(store_only) << 15U) |
         ((base & 31U) << 5U) | (result & 31U);
}

[[nodiscard]] constexpr bool is_cached_gpr(unsigned reg) noexcept {
  return reg == 3U || reg == 4U || reg == 5U || reg == 6U ||
         reg == 7U || reg == 12U || reg == 13U || reg == 17U;
}

[[nodiscard]] constexpr unsigned destination(std::uint32_t instruction) {
  return instruction & 31U;
}

[[nodiscard]] constexpr unsigned left(std::uint32_t instruction) {
  return (instruction >> 5U) & 31U;
}

[[nodiscard]] constexpr unsigned right(std::uint32_t instruction) {
  return (instruction >> 16U) & 31U;
}

struct RegisterMap final {
  std::array<std::optional<unsigned>, 32> host{};

  void bind(unsigned guest, unsigned native) {
    require(native >= first_cached_vector && native <= last_cached_vector,
            "generated operation escaped the v16-v31 cache");
    auto &binding = host[guest];
    if (binding.has_value())
      require(*binding == native,
              "guest vector changed host binding inside the block");
    else
      binding = native;
  }
};

void bind_instruction(RegisterMap &mapping, std::uint32_t guest,
                      std::uint32_t native) {
  if (left(guest) != right(guest))
    require(left(native) != right(native),
            "distinct vector inputs share one host register");
  mapping.bind(destination(guest), destination(native));
  mapping.bind(left(guest), left(native));
  mapping.bind(right(guest), right(native));
}

[[nodiscard]] bool
is_direct_remapped_run(std::span<const std::uint32_t> native,
                       std::span<const std::uint32_t> guest) {
  if (native.size() < guest.size())
    return false;

  for (std::size_t index = 0; index < guest.size(); ++index)
    if ((native[index] & ~register_mask) !=
        (guest[index] & ~register_mask))
      return false;

  RegisterMap mapping;
  for (std::size_t index = 0; index < guest.size(); ++index)
    bind_instruction(mapping, guest[index], native[index]);

  /*
   * Every result is the next operation's left input. Consecutive matching
   * words and identical bindings prove there is no CpuState vector
   * store/load pair between the guest operations.
   */
  for (std::size_t index = 1; index < guest.size(); ++index)
    require(destination(native[index - 1]) == left(native[index]),
            "vector dependency was not kept resident");
  return true;
}

void vector_dependency_chain_is_direct_and_resident() {
  /*
   * These unrestricted-register 128-bit bitwise forms are part of baseline
   * Advanced SIMD. The two long-lived inputs also prevent a trivial
   * single-temporary lowering from satisfying the dependency checks.
   */
  constexpr std::array guest{
      bitwise(4, 0, 1, 2), // eor v0.16b, v1.16b, v2.16b
      bitwise(2, 3, 0, 2), // orr v3.16b, v0.16b, v2.16b
      bitwise(0, 4, 3, 1), // and v4.16b, v3.16b, v1.16b
      bitwise(1, 5, 4, 2), // bic v5.16b, v4.16b, v2.16b
      bitwise(3, 6, 5, 1), // orn v6.16b, v5.16b, v1.16b
  };

  const auto compiled = jit_test::compile_block(code_base, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the complete vector dependency chain");
  require(compiled.hot_words <= compiled.words.size(),
          "JIT hot-word boundary exceeds generated vector code");
  require(compiled.hot_words >= guest.size(),
          "JIT vector hot path is shorter than the guest operation run");

  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  bool found = false;
  for (std::size_t start = 0; start + guest.size() <= hot.size(); ++start) {
    if (!is_direct_remapped_run(hot.subspan(start, guest.size()), guest))
      continue;
    require(!found, "JIT emitted the vector dependency chain more than once");
    found = true;
  }
  require(found,
          "JIT did not emit one consecutive native word per vector operation");
}

void specialized_vector_dependency_chain_is_direct_and_resident() {
  /*
   * The byte-lane form is part of baseline Advanced SIMD, so this code-shape
   * regression remains useful even when optional host features are absent.
   */
  constexpr std::array guest{
      bitwise(4, 5, 1, 2),
      polynomial_multiply(6, 5, 2),
      bitwise(2, 7, 6, 1),
  };

  const auto compiled = jit_test::compile_block(code_base + 0x1000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the specialized vector dependency chain");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  bool found = false;
  for (std::size_t start = 0; start + guest.size() <= hot.size(); ++start) {
    if (!is_direct_remapped_run(hot.subspan(start, guest.size()), guest))
      continue;
    require(!found,
            "JIT emitted the specialized vector chain more than once");
    found = true;
  }
  require(found,
          "specialized vector operation staged through fixed registers");
}

void require_direct_three_vector_operation(std::string_view name,
                                           std::uint32_t operation,
                                           std::uint64_t pc) {
  constexpr unsigned destination_guest = 0U;
  constexpr unsigned left_guest = 1U;
  constexpr unsigned right_guest = 2U;
  const std::array guest{
      bitwise(4, destination_guest, 3, 4),
      bitwise(4, left_guest, 5, 6),
      bitwise(4, right_guest, 7, 8),
      operation | (right_guest << 16U) | (left_guest << 5U) |
          destination_guest,
      bitwise(2, 9, destination_guest, 10),
  };
  const auto compiled = jit_test::compile_block(pc, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile a direct vector dependency chain");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::array<std::uint32_t, 3> producers{};
  std::size_t producer_count{};
  std::optional<std::uint32_t> native_operation;
  std::optional<std::uint32_t> consumer;
  for (const auto word : hot) {
    if ((word & ~register_mask) == (guest[0] & ~register_mask)) {
      require(producer_count < producers.size(),
              "JIT emitted an unexpected vector-chain producer");
      producers[producer_count++] = word;
    }
    if ((word & ~register_mask) == (guest[3] & ~register_mask)) {
      require(!native_operation.has_value(),
              "JIT emitted a vector dependency operation twice");
      native_operation = word;
    }
    if ((word & ~register_mask) == (guest[4] & ~register_mask)) {
      require(!consumer.has_value(),
              "JIT emitted a vector dependency consumer twice");
      consumer = word;
    }
  }
  if (producer_count != producers.size() ||
      !native_operation.has_value() || !consumer.has_value()) {
    std::fprintf(stderr, "%.*s is missing a direct native operation\n",
                 static_cast<int>(name.size()), name.data());
    std::abort();
  }

  require(destination(producers[0]) == destination(*native_operation),
          "vector destination was staged through a fixed register");
  require(destination(producers[1]) == left(*native_operation),
          "vector first source was staged through a fixed register");
  require(destination(producers[2]) == right(*native_operation),
          "vector second source was staged through a fixed register");
  require(destination(*native_operation) == left(*consumer),
          "vector result was not retained for its consumer");
  require(destination(*native_operation) >= first_cached_vector &&
              destination(*native_operation) <= last_cached_vector &&
              left(*native_operation) >= first_cached_vector &&
              left(*native_operation) <= last_cached_vector &&
              right(*native_operation) >= first_cached_vector &&
              right(*native_operation) <= last_cached_vector,
          "vector operation escaped the resident vector cache");
}

void remaining_floating_families_name_resident_vectors_directly() {
  require_direct_three_vector_operation(
      "vector horizontal pairwise add", 0x6e20'd400U,
      code_base + 0x20'000U);
  require_direct_three_vector_operation(
      "scalar reciprocal step", 0x5e20'fc00U,
      code_base + 0x21'000U);

  const auto &host = uml::backend::host_arm_features();
  if (host.has(uml::backend::ArmFeature::fp8))
    require_direct_three_vector_operation(
        "eight-bit convert down", 0x0e40'f400U,
        code_base + 0x24'000U);
  if (host.has(uml::backend::ArmFeature::fp8dot4))
    require_direct_three_vector_operation(
        "eight-bit dot accumulator", 0x0e00'fc00U,
        code_base + 0x25'000U);
  if (host.has(uml::backend::ArmFeature::f8f32mm))
    require_direct_three_vector_operation(
        "eight-bit matrix accumulator", 0x6e80'ec00U,
        code_base + 0x26'000U);
}

void remaining_simd_arithmetic_families_name_resident_vectors_directly() {
  const auto &host = uml::backend::host_arm_features();
  if (!host.advanced_simd)
    return;

  require_direct_three_vector_operation(
      "widening accumulator", 0x0e20'5000U,
      code_base + 0x29'000U);
  require_direct_three_vector_operation(
      "word indexed integer accumulator", 0x6f80'0000U,
      code_base + 0x2a'000U);
  require_direct_three_vector_operation(
      "table lookup", 0x4e00'0000U,
      code_base + 0x2b'000U);
  require_direct_three_vector_operation(
      "vector permute", 0x4e00'2800U,
      code_base + 0x2c'000U);

  if (host.floating_point)
    require_direct_three_vector_operation(
        "word indexed floating accumulator", 0x4f80'1000U,
        code_base + 0x2d'000U);
  if (host.has(uml::backend::ArmFeature::bf16)) {
    require_direct_three_vector_operation(
        "bfloat vector dot accumulator", 0x6e40'fc00U,
        code_base + 0x2e'000U);
    require_direct_three_vector_operation(
        "bfloat indexed dot accumulator", 0x4f40'f000U,
        code_base + 0x2f'000U);
    require_direct_three_vector_operation(
        "bfloat vector multiply-long accumulator", 0x6ec0'fc00U,
        code_base + 0x30'000U);
    require_direct_three_vector_operation(
        "bfloat matrix accumulator", 0x6e40'ec00U,
        code_base + 0x31'000U);
  }
  if (host.has(uml::backend::ArmFeature::f16f32dot) && host.afp) {
    require_direct_three_vector_operation(
        "half vector dot accumulator", 0x4e80'fc00U,
        code_base + 0x32'000U);
    require_direct_three_vector_operation(
        "half indexed dot accumulator", 0x4f40'9000U,
        code_base + 0x33'000U);
  }
  if (host.has(uml::backend::ArmFeature::fhm) && host.afp)
    require_direct_three_vector_operation(
        "half vector fused-long accumulator", 0x4e20'ec00U,
        code_base + 0x34'000U);
  if (host.has(uml::backend::ArmFeature::f16f32mm) && host.afp)
    require_direct_three_vector_operation(
        "half widening matrix accumulator", 0x4e40'ec00U,
        code_base + 0x35'000U);
  if (host.has(uml::backend::ArmFeature::dotprod)) {
    require_direct_three_vector_operation(
        "integer vector dot accumulator", 0x4e80'9400U,
        code_base + 0x36'000U);
    require_direct_three_vector_operation(
        "integer indexed dot accumulator", 0x4f80'e000U,
        code_base + 0x37'000U);
  }
  if (host.has(uml::backend::ArmFeature::i8mm))
    require_direct_three_vector_operation(
        "integer matrix accumulator", 0x4e80'a400U,
        code_base + 0x38'000U);
  if (host.has(uml::backend::ArmFeature::lut))
    require_direct_three_vector_operation(
        "single-table LUT", 0x4e40'2000U,
        code_base + 0x39'000U);
}

void require_restricted_element_operation(std::string_view name,
                                          std::uint32_t operation,
                                          std::uint64_t pc) {
  constexpr std::uint32_t restricted_register_mask =
      (15U << 16U) | (31U << 5U) | 31U;
  constexpr unsigned destination_guest = 0U;
  constexpr unsigned left_guest = 1U;
  constexpr unsigned element_guest = 2U;
  const std::array guest{
      bitwise(4, destination_guest, 3U, 4U),
      bitwise(4, left_guest, 5U, 6U),
      bitwise(4, element_guest, 7U, 8U),
      operation | (element_guest << 16U) | (left_guest << 5U) |
          destination_guest,
      bitwise(2, 9U, destination_guest, 10U),
  };
  const auto compiled = jit_test::compile_block(pc, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile a restricted-element dependency chain");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::array<std::uint32_t, 3> producers{};
  std::size_t producer_count{};
  std::optional<std::uint32_t> native_operation;
  std::optional<std::uint32_t> consumer;
  for (const auto word : hot) {
    if ((word & ~register_mask) == (guest[0] & ~register_mask)) {
      require(producer_count < producers.size(),
              "JIT emitted an unexpected restricted-chain producer");
      producers[producer_count++] = word;
    }
    if ((word & ~restricted_register_mask) ==
        (guest[3] & ~restricted_register_mask)) {
      require(!native_operation.has_value(),
              "JIT emitted a restricted-element operation twice");
      native_operation = word;
    }
    if ((word & ~register_mask) == (guest[4] & ~register_mask))
      consumer = word;
  }
  if (producer_count != producers.size() ||
      !native_operation.has_value() || !consumer.has_value()) {
    std::fprintf(stderr,
                 "%.*s is missing its restricted native operation\n",
                 static_cast<int>(name.size()), name.data());
    std::abort();
  }

  require(destination(producers[0]) == destination(*native_operation),
          "restricted operation did not use its resident destination");
  require(destination(producers[1]) == left(*native_operation),
          "restricted operation did not use its resident first source");
  require(destination(producers[2]) == right(*native_operation),
          "restricted element did not use its pinned low-register binding");
  require(right(*native_operation) == 6U ||
              right(*native_operation) == 7U,
          "restricted element escaped the q6/q7 resident bank");
  require(destination(*native_operation) == left(*consumer),
          "restricted operation result was not retained for its consumer");
}

void restricted_element_families_use_the_low_resident_bank() {
  const auto &host = uml::backend::host_arm_features();
  if (!host.advanced_simd)
    return;

  require_restricted_element_operation(
      "halfword indexed integer accumulator", 0x6f40'0000U,
      code_base + 0x3a'000U);
  if (host.fp16 && host.floating_point)
    require_restricted_element_operation(
        "half indexed floating accumulator", 0x4f00'1000U,
        code_base + 0x3b'000U);
  if (host.has(uml::backend::ArmFeature::fhm) && host.afp)
    require_restricted_element_operation(
        "half indexed fused-long accumulator", 0x4f80'0000U,
        code_base + 0x3c'000U);
  if (host.has(uml::backend::ArmFeature::bf16))
    require_restricted_element_operation(
        "bfloat indexed multiply-long accumulator", 0x4fc0'f000U,
        code_base + 0x3d'000U);
  if (host.has(uml::backend::ArmFeature::fp8dot2))
    require_restricted_element_operation(
        "eight-bit indexed dot-to-half accumulator", 0x0f40'0000U,
        code_base + 0x49'000U);
  if (host.has(uml::backend::ArmFeature::fp8fma)) {
    require_restricted_element_operation(
        "eight-bit indexed multiply-to-half accumulator", 0x0fc0'0000U,
        code_base + 0x4a'000U);
    require_restricted_element_operation(
        "eight-bit indexed multiply-to-single accumulator", 0x2f00'8000U,
        code_base + 0x4b'000U);
  }
}

void constrained_frequency_selects_two_stable_pins() {
  if (!uml::backend::host_arm_features().advanced_simd)
    return;

  constexpr std::uint32_t operand_mask =
      (15U << 16U) | (31U << 5U) | 31U;
  constexpr auto indexed_mla =
      [](unsigned destination, unsigned first, unsigned element) {
        return 0x6f40'0000U | ((element & 15U) << 16U) |
               ((first & 31U) << 5U) | (destination & 31U);
      };
  /*
   * v7 is most frequent and must take q6. v9 and v8 tie; v9 appears first
   * and must take q7. The unpinned v8 identity remains on the q2 fallback
   * without shortening the block.
   */
  constexpr std::array guest{
      indexed_mla(0U, 1U, 9U),
      indexed_mla(0U, 1U, 8U),
      indexed_mla(0U, 1U, 7U),
      indexed_mla(0U, 1U, 8U),
      indexed_mla(0U, 1U, 9U),
      indexed_mla(0U, 1U, 7U),
      indexed_mla(0U, 1U, 7U),
  };
  constexpr std::array expected_hosts{7U, 2U, 6U, 2U, 7U, 6U, 6U};
  const auto compiled =
      jit_test::compile_block(code_base + 0x4c'000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "third constrained identity shortened the JIT block");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  std::size_t operation{};
  for (const auto word : hot) {
    if (operation == guest.size() ||
        (word & ~operand_mask) != (guest[operation] & ~operand_mask))
      continue;
    require(right(word) == expected_hosts[operation],
            "constrained source frequency/tie selection changed");
    ++operation;
  }
  require(operation == guest.size(),
          "frequency-selection sequence omitted a native operation");
}

void constrained_pin_is_visible_to_an_earlier_producer() {
  if (!uml::backend::host_arm_features().advanced_simd)
    return;

  constexpr auto producer = bitwise(4U, 9U, 1U, 2U);
  constexpr auto constrained =
      0x6f40'0000U | (9U << 16U) | (3U << 5U);
  constexpr auto consumer = bitwise(2U, 4U, 9U, 5U);
  constexpr std::array guest{producer, constrained, consumer};
  const auto compiled =
      jit_test::compile_block(code_base + 0x4e'000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "prebound constrained sequence did not remain one block");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::optional<std::uint32_t> native_producer;
  std::optional<std::uint32_t> native_constrained;
  std::optional<std::uint32_t> native_consumer;
  for (const auto word : hot) {
    if ((word & ~register_mask) == (producer & ~register_mask))
      native_producer = word;
    if ((word & ~((15U << 16U) | (31U << 5U) | 31U)) ==
        (constrained & ~((15U << 16U) | (31U << 5U) | 31U)))
      native_constrained = word;
    if ((word & ~register_mask) == (consumer & ~register_mask))
      native_consumer = word;
  }
  require(native_producer.has_value() &&
              native_constrained.has_value() &&
              native_consumer.has_value(),
          "prebound constrained sequence omitted a native operation");
  require(destination(*native_producer) == 6U,
          "translation did not prebind the constrained identity in q6");
  require(right(*native_constrained) == 6U,
          "constrained use did not reuse the q6 binding");
  require(left(*native_consumer) == 6U,
          "ordinary consumer did not reuse the q6 binding");
}

void third_constrained_identity_uses_q2_when_high_bank_is_full() {
  if (!uml::backend::host_arm_features().advanced_simd)
    return;

  constexpr std::uint32_t constrained_mask =
      (15U << 16U) | (31U << 5U) | 31U;
  constexpr auto indexed_mla =
      [](unsigned destination, unsigned first, unsigned element) {
        return 0x6f40'0000U | ((element & 15U) << 16U) |
               ((first & 31U) << 5U) | (destination & 31U);
      };
  constexpr std::array fill_guests{
      0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U,
      8U, 9U, 10U, 11U, 12U, 16U, 17U, 31U,
  };
  std::array<std::uint32_t, 2U + fill_guests.size() + 1U> guest{};
  guest[0] = indexed_mla(13U, 13U, 13U);
  guest[1] = indexed_mla(14U, 14U, 14U);
  for (std::size_t index = 0; index < fill_guests.size(); ++index)
    guest[index + 2U] =
        modified_immediate(false, false, index + 1U, fill_guests[index]);
  const auto target = indexed_mla(31U, 31U, 15U);
  guest.back() = target;

  const auto compiled =
      jit_test::compile_block(code_base + 0x50'000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "third constrained identity split a full-cache block");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  std::optional<std::uint32_t> native;
  for (const auto word : hot)
    if ((word & ~constrained_mask) == (target & ~constrained_mask))
      native = word;
  require(native.has_value(),
          "full-cache third constrained operation was not emitted");
  require(left(*native) == 31U && destination(*native) == 31U &&
              right(*native) == 2U,
          "third constrained identity did not use the q2 fallback");
}

void constrained_planning_stops_at_a_terminal() {
  if (!uml::backend::host_arm_features().advanced_simd)
    return;

  constexpr std::uint32_t constrained_mask =
      (15U << 16U) | (31U << 5U) | 31U;
  constexpr auto indexed_mla =
      [](unsigned destination, unsigned first, unsigned element) {
        return 0x6f40'0000U | ((element & 15U) << 16U) |
               ((first & 31U) << 5U) | (destination & 31U);
      };
  constexpr auto before = indexed_mla(0U, 1U, 9U);
  constexpr auto after = indexed_mla(2U, 3U, 8U);
  constexpr std::array guest{
      before, 0xd400'0001U, after, after, after, after,
  };
  const auto compiled =
      jit_test::compile_block(code_base + 0x51'000U, guest);
  require(compiled.guest_instructions == 2U,
          "terminal did not bound the translated constrained prefix");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  std::optional<std::uint32_t> native;
  for (const auto word : hot)
    if ((word & ~constrained_mask) == (before & ~constrained_mask))
      native = word;
  require(native.has_value(),
          "pre-terminal constrained operation was not emitted");
  require(right(*native) == 6U,
          "post-terminal frequency changed the translated prefix binding");
}

void future_constrained_word_does_not_expand_a_terminal_prefix() {
  if (!uml::backend::host_arm_features().advanced_simd)
    return;

  constexpr auto ordinary = bitwise(4U, 9U, 1U, 2U);
  constexpr std::uint32_t system_call = 0xd400'0001U;
  constexpr auto future_constrained =
      0x6f40'0000U | (9U << 16U) | (3U << 5U);
  constexpr std::array prefix{ordinary, system_call};
  constexpr std::array fetched{ordinary, system_call, future_constrained};
  const auto expected =
      jit_test::compile_block(code_base + 0x4f'000U, prefix);
  const auto actual =
      jit_test::compile_block(code_base + 0x4f'000U, fetched);
  require(expected.guest_instructions == prefix.size() &&
              actual.guest_instructions == prefix.size(),
          "system-call prefix retired an unexpected instruction count");
  require(actual.words.size() == expected.words.size() &&
              actual.hot_words == expected.hot_words &&
              actual.internal_entry_word == expected.internal_entry_word &&
              actual.memory_write_mask == expected.memory_write_mask &&
              actual.direct_memory_write_mask ==
                  expected.direct_memory_write_mask &&
              actual.accesses_fp_state == expected.accesses_fp_state &&
              actual.native_fp == expected.native_fp &&
              actual.native_fpmr == expected.native_fpmr,
          "future constrained word expanded the terminal prefix");
}

void unrestricted_fp8_binding_order_is_unchanged() {
  const auto &host = uml::backend::host_arm_features();
  if (!host.has(uml::backend::ArmFeature::fp8dot4))
    return;

  constexpr std::uint32_t guest =
      0x0e00'fc00U | (2U << 16U) | (1U << 5U);
  const std::array block{guest};
  const auto compiled =
      jit_test::compile_block(code_base + 0x4d'000U, block);
  require(compiled.guest_instructions == block.size(),
          "unrestricted eight-bit dot form did not compile");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  std::optional<std::uint32_t> native;
  for (const auto word : hot)
    if ((word & ~register_mask) == (guest & ~register_mask))
      native = word;
  require(native.has_value(),
          "unrestricted eight-bit dot form omitted its native word");
  require(left(*native) == 16U && right(*native) == 17U &&
              destination(*native) == 18U,
          "unrestricted eight-bit operand allocation order changed");
}

void require_ordinary_vector_memory_chain(
    std::span<const std::uint32_t> guest, std::uint64_t pc) {
  const auto compiled = jit_test::compile_block(pc, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the complete vector-memory chain");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::optional<std::uint32_t> producer;
  std::optional<std::uint32_t> consumer;
  std::optional<std::uint32_t> load;
  std::array<std::uint32_t, 2> stores{};
  std::size_t store_count = 0;
  for (const auto word : hot) {
    if ((word & ~register_mask) == (guest[0] & ~register_mask))
      producer = word;
    if ((word & ~register_mask) == (guest[4] & ~register_mask))
      consumer = word;
    if ((word & ~31U) == 0x3d80'0100U) {
      require(store_count < stores.size(),
              "JIT emitted an unexpected direct vector store");
      stores[store_count++] = word;
    }
    if ((word & ~31U) == 0x3dc0'0100U) {
      require(!load.has_value(),
              "JIT emitted an unexpected direct vector load");
      load = word;
    }
  }

  require(producer.has_value(),
          "JIT did not emit the resident vector producer");
  require(consumer.has_value(),
          "JIT did not emit the resident vector consumer");
  require(store_count == stores.size(),
          "JIT did not emit both direct vector stores");
  require(load.has_value(), "JIT did not emit the direct vector load");

  const auto loaded_host = destination(*load);
  require(destination(stores[0]) == destination(*producer),
          "resident vector producer was staged before its store");
  require(loaded_host == destination(stores[1]),
          "loaded vector was staged before its following store");
  require(loaded_host == left(*consumer),
          "loaded vector was staged before its dependent operation");
  require(right(*producer) == right(*consumer),
          "live vector input changed host binding across memory operations");
  require(destination(stores[0]) >= first_cached_vector &&
              destination(stores[0]) <= last_cached_vector &&
              loaded_host >= first_cached_vector &&
              loaded_host <= last_cached_vector,
          "ordinary vector memory data operand escaped the resident cache");
}

void ordinary_memory_names_resident_vector_registers_directly() {
  constexpr std::array unsigned_guest{
      bitwise(4, 5, 1, 2),                  // eor v5.16b, v1.16b, v2.16b
      vector_unsigned_memory(false, 10, 5), // str q5, [x10]
      vector_unsigned_memory(true, 10, 6),  // ldr q6, [x10]
      vector_unsigned_memory(false, 10, 6), // str q6, [x10]
      bitwise(2, 7, 6, 2),                  // orr v7.16b, v6.16b, v2.16b
  };
  require_ordinary_vector_memory_chain(unsigned_guest,
                                       code_base + 0x2000U);

  constexpr std::array unscaled_guest{
      bitwise(4, 5, 1, 2), // eor v5.16b, v1.16b, v2.16b
      0x3c80'0145U,         // stur q5, [x10]
      0x3cc0'0146U,         // ldur q6, [x10]
      0x3c80'0146U,         // stur q6, [x10]
      bitwise(2, 7, 6, 2), // orr v7.16b, v6.16b, v2.16b
  };
  require_ordinary_vector_memory_chain(unscaled_guest,
                                       code_base + 0x3000U);
}

void literal_load_names_its_resident_vector_directly() {
  constexpr std::array guest{
      0x9c00'0005U,         // ldr q5, literal
      bitwise(2, 6, 5, 2), // orr v6.16b, v5.16b, v2.16b
  };
  const auto compiled = jit_test::compile_block(code_base + 0x4000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile the vector literal-load dependency");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::optional<std::uint32_t> load;
  std::optional<std::uint32_t> consumer;
  for (const auto word : hot) {
    if ((word & ~31U) == 0x3dc0'0100U)
      load = word;
    if ((word & ~register_mask) == (guest[1] & ~register_mask))
      consumer = word;
  }
  require(load.has_value(), "JIT did not emit the native vector literal load");
  require(consumer.has_value(),
          "JIT did not emit the vector literal load's cached consumer");
  require(destination(*load) == left(*consumer),
          "vector literal load was staged before its dependent operation");
  require(destination(*load) >= first_cached_vector &&
              destination(*load) <= last_cached_vector,
          "vector literal load destination escaped the resident cache");
}

void atomic_floating_names_resident_vectors_directly() {
  if (!uml::backend::host_arm_features().has(
          uml::backend::ArmFeature::lsfe))
    return;

  const auto require_returning_form = [](bool alias, std::uint64_t pc) {
    const unsigned result = alias ? 5U : 6U;
    const std::array guest{
        bitwise(2, 5, 1, 2),
        atomic_floating(false, 10, 5, result),
        bitwise(2, 7, result, 2),
    };
    const auto compiled = jit_test::compile_block(pc, guest);
    require(compiled.guest_instructions == guest.size(),
            "JIT did not compile the atomic floating-memory chain");
    const auto hot = std::span<const std::uint32_t>{compiled.words}.first(
        compiled.hot_words);

    std::optional<std::uint32_t> producer;
    std::optional<std::uint32_t> atomic;
    std::optional<std::uint32_t> consumer;
    for (const auto word : hot) {
      if ((word & ~register_mask) == (guest[0] & ~register_mask))
        producer = word;
      if ((word & ~register_mask) == (guest[1] & ~register_mask))
        atomic = word;
      if ((word & ~register_mask) == (guest[2] & ~register_mask))
        consumer = word;
    }

    require(producer.has_value(), "JIT did not emit the cached source");
    require(atomic.has_value(),
            "JIT did not emit the native atomic floating-memory operation");
    require(consumer.has_value(), "JIT did not emit the cached result user");
    require(left(*atomic) == 8U,
            "native atomic floating-memory base is not translated x8");
    require(right(*atomic) == destination(*producer),
            "atomic floating-memory source was staged through v0");
    require(destination(*atomic) == left(*consumer),
            "atomic floating-memory result was staged through v1");
    require(right(*atomic) >= first_cached_vector &&
                right(*atomic) <= last_cached_vector &&
                destination(*atomic) >= first_cached_vector &&
                destination(*atomic) <= last_cached_vector,
            "atomic floating-memory operand escaped the resident cache");
    require((right(*atomic) == destination(*atomic)) == alias,
            "atomic floating-memory source/result alias was not preserved");
  };

  require_returning_form(false, code_base + 0x5000U);
  require_returning_form(true, code_base + 0x6000U);

  constexpr std::array store_guest{
      bitwise(2, 5, 1, 2),
      atomic_floating(true, 10, 5, 31),
  };
  const auto compiled =
      jit_test::compile_block(code_base + 0x7000U, store_guest);
  require(compiled.guest_instructions == store_guest.size(),
          "JIT did not compile the store-only atomic floating-memory chain");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  std::optional<std::uint32_t> producer;
  std::optional<std::uint32_t> atomic;
  for (const auto word : hot) {
    if ((word & ~register_mask) == (store_guest[0] & ~register_mask))
      producer = word;
    if ((word & ~register_mask) == (store_guest[1] & ~register_mask))
      atomic = word;
  }
  require(producer.has_value(),
          "JIT did not emit the store-only cached source");
  require(atomic.has_value(),
          "JIT did not emit the store-only native operation");
  require(right(*atomic) == destination(*producer),
          "store-only atomic floating-memory source was staged through v0");
  require(destination(*atomic) == 31U,
          "store-only atomic floating-memory result is not encoded as v31");
}

[[nodiscard]] constexpr bool
is_cpu_state_vector_load(std::uint32_t instruction) noexcept {
  /*
   * LDR Qt, [x15, #imm] is the only generated instruction that imports a
   * guest vector from CpuState. Ignore Rt and the scaled immediate.
   */
  return (instruction & 0xffc0'03e0U) == 0x3dc0'01e0U;
}

[[nodiscard]] std::optional<std::uint32_t>
find_native_operation(std::span<const std::uint32_t> hot,
                      std::uint32_t guest,
                      std::uint32_t operand_mask) {
  std::optional<std::uint32_t> result;
  for (const auto word : hot) {
    if ((word & ~operand_mask) != (guest & ~operand_mask))
      continue;
    require(!result.has_value(),
            "JIT emitted a checked native operation more than once");
    result = word;
  }
  return result;
}

void unrestricted_low_pressure_shape_is_unchanged() {
  constexpr std::array guest{bitwise(4U, 0U, 1U, 2U)};
  const auto compiled =
      jit_test::compile_block(code_base + 0x52'000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "low-pressure unrestricted operation did not compile");
  require(compiled.hot_words == 22U && compiled.words.size() == 24U,
          "low-pressure unrestricted code size changed");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  const auto native =
      find_native_operation(hot, guest.front(), register_mask);
  require(native.has_value(),
          "low-pressure unrestricted operation omitted its native word");
  require(left(*native) == 16U && right(*native) == 17U &&
              destination(*native) == 18U,
          "unrestricted low-pressure register order changed");
  require(std::count_if(hot.begin(), hot.end(),
                        is_cpu_state_vector_load) == 2,
          "unrestricted low-pressure live-in shape changed");
}

void ordinary_pressure_uses_both_unbound_low_slots() {
  std::array<std::uint32_t, 19> guest{};
  for (unsigned index = 0; index < 18U; ++index)
    guest[index] =
        modified_immediate(false, false, 0x40U + index, index);
  guest.back() = bitwise(4U, 16U, 16U, 17U);

  const auto compiled =
      jit_test::compile_block(code_base + 0x53'000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "unconstrained 18-vector pressure block did not compile");
  /*
   * The prior high-bank-only allocation emitted 58 hot / 60 total words for
   * this block. Keeping V16/V17 resident removes two imports and one duplicate
   * state write; tail alignment absorbs one of the three total-word savings.
   */
  require(compiled.hot_words == 55U && compiled.words.size() == 58U,
          "ordinary q6/q7 capacity lost its exact code-size reduction");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  for (unsigned index = 0; index < 18U; ++index) {
    const auto native =
        find_native_operation(hot, guest[index], 31U);
    require(native.has_value(),
            "ordinary pressure block omitted a modified immediate");
    const unsigned expected =
        index < 16U ? 16U + index : 6U + (index - 16U);
    require(destination(*native) == expected,
            "ordinary pressure allocation order changed");
  }

  const auto consumer =
      find_native_operation(hot, guest.back(), register_mask);
  require(consumer.has_value(),
          "ordinary low-slot consumer omitted its native word");
  require(left(*consumer) == 6U && right(*consumer) == 7U &&
              destination(*consumer) == 6U,
          "ordinary q6/q7 values were staged or spilled before reuse");
  require(std::none_of(hot.begin(), hot.end(),
                       is_cpu_state_vector_load),
          "ordinary q6/q7 reuse reloaded CpuState");
}

void one_constrained_pin_leaves_one_low_slot_for_ordinary_pressure() {
  constexpr std::array ordinary_guests{
      0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U,
      8U, 9U, 10U, 11U, 12U, 13U, 14U, 16U,
  };
  std::array<std::uint32_t, 19> guest{};
  guest[0] = 0x4f40'8000U | (15U << 16U) |
             (15U << 5U) | 15U;
  for (std::size_t index = 0; index < ordinary_guests.size(); ++index)
    guest[index + 1U] =
        modified_immediate(false, false, 0x60U + index,
                           ordinary_guests[index]);
  guest[17] = modified_immediate(false, false, 0xa4U, 17U);
  guest[18] = bitwise(4U, 17U, 17U, 0U);

  const auto compiled =
      jit_test::compile_block(code_base + 0x54'000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "single-pin pressure block did not compile");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  const auto constrained =
      find_native_operation(hot, guest.front(),
                            constrained_register_mask);
  const auto low_definition =
      find_native_operation(hot, guest[17], 31U);
  const auto low_consumer =
      find_native_operation(hot, guest[18], register_mask);
  require(constrained.has_value() && low_definition.has_value() &&
              low_consumer.has_value(),
          "single-pin pressure block omitted a native operation");
  require(left(*constrained) == 6U && right(*constrained) == 6U &&
              destination(*constrained) == 6U,
          "preselected constrained identity lost q6");
  require(destination(*low_definition) == 7U &&
              left(*low_consumer) == 7U &&
              destination(*low_consumer) == 7U,
          "single constrained pin did not leave q7 for ordinary pressure");
}

void two_constrained_pins_leave_no_low_slot_for_ordinary_pressure() {
  constexpr std::array ordinary_guests{
      0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U,
      8U, 9U, 10U, 11U, 12U, 13U, 16U, 17U,
  };
  std::array<std::uint32_t, 19> guest{};
  guest[0] = 0x4f40'8000U | (14U << 16U) |
             (14U << 5U) | 14U;
  guest[1] = 0x4f40'8000U | (15U << 16U) |
             (15U << 5U) | 15U;
  for (std::size_t index = 0; index < ordinary_guests.size(); ++index)
    guest[index + 2U] =
        modified_immediate(false, false, 0x80U + index,
                           ordinary_guests[index]);
  guest.back() = modified_immediate(false, false, 0xd2U, 18U);

  const auto compiled =
      jit_test::compile_block(code_base + 0x55'000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "two-pin pressure block did not compile");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  std::array<std::uint32_t, 2> pins{};
  std::size_t pin_count{};
  for (const auto word : hot) {
    if ((word & ~constrained_register_mask) !=
        (guest[0] & ~constrained_register_mask))
      continue;
    require(pin_count < pins.size(),
            "two-pin pressure block emitted an extra constrained word");
    pins[pin_count++] = word;
  }
  const auto overflow =
      find_native_operation(hot, guest.back(), 31U);
  require(pin_count == pins.size() && overflow.has_value(),
          "two-pin pressure block omitted a native operation");
  require(right(pins[0]) == 6U && right(pins[1]) == 7U,
          "preselected constrained identities changed low bindings");
  require(destination(*overflow) == 0U,
          "ordinary pressure consumed a constrained low slot");
}

void require_vector_live_ins(std::string_view name,
                            std::uint32_t instruction,
                            std::size_t expected) {
  const std::array guest{instruction};
  const auto compiled = jit_test::compile_block(code_base + 0x1000U, guest);
  require(compiled.guest_instructions == 1U,
          "JIT did not compile a vector live-in test instruction");
  require(compiled.hot_words <= compiled.words.size(),
          "JIT hot-word boundary exceeds generated vector live-in code");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
  std::size_t actual{};
  for (const auto word : hot)
    actual += is_cpu_state_vector_load(word);
  if (actual != expected) {
    std::fprintf(stderr,
                 "%.*s imported %zu guest vectors; expected %zu\n",
                 static_cast<int>(name.size()), name.data(), actual,
                 expected);
    std::abort();
  }
}

void modified_immediates_name_resident_destination_directly() {
  const auto check = [](std::string_view name, std::uint32_t instruction,
                        std::size_t expected_live_ins, std::uint64_t pc) {
    const std::array guest{instruction};
    const auto compiled = jit_test::compile_block(pc, guest);
    require(compiled.guest_instructions == guest.size(),
            "JIT did not compile a modified-immediate instruction");
    const auto hot =
        std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
    std::optional<std::uint32_t> native;
    std::size_t live_ins{};
    for (const auto word : hot) {
      live_ins += is_cpu_state_vector_load(word);
      if ((word & ~31U) != (instruction & ~31U))
        continue;
      require(!native.has_value(),
              "JIT emitted a modified-immediate instruction twice");
      native = word;
    }
    if (!native.has_value()) {
      std::fprintf(stderr, "%.*s was not emitted directly\n",
                   static_cast<int>(name.size()), name.data());
      std::abort();
    }
    require(destination(*native) >= first_cached_vector &&
                destination(*native) <= last_cached_vector,
            "modified-immediate destination escaped the resident cache");
    require(live_ins == expected_live_ins,
            "modified-immediate imported the wrong destination state");
  };

  check("modified-immediate overwrite",
        modified_immediate(false, false, 0xa6U, 5U), 0U,
        code_base + 0x24'000U);
  check("modified-immediate OR combine",
        modified_immediate(true, false, 0x93U, 6U), 1U,
        code_base + 0x25'000U);
  check("modified-immediate clear combine",
        modified_immediate(true, true, 0x57U, 7U), 1U,
        code_base + 0x26'000U);
}

void modified_immediate_full_cache_fallback_is_exact() {
  constexpr auto &fillers = full_vector_cache_fillers;
  const auto check = [&](std::string_view name, std::uint32_t target,
                         std::size_t expected_live_ins, std::uint64_t pc) {
    std::array<std::uint32_t, fillers.size() + 1U> guest{};
    std::copy(fillers.begin(), fillers.end(), guest.begin());
    guest.back() = target;
    const auto compiled = jit_test::compile_block(pc, guest);
    require(compiled.guest_instructions == guest.size(),
            "JIT did not compile the full-cache immediate sequence");
    const auto hot =
        std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
    std::optional<std::uint32_t> native;
    std::size_t live_ins{};
    for (const auto word : hot) {
      live_ins += is_cpu_state_vector_load(word);
      if ((word & ~31U) == (target & ~31U))
        native = word;
    }
    if (!native.has_value()) {
      std::fprintf(stderr, "%.*s fallback was not emitted directly\n",
                   static_cast<int>(name.size()), name.data());
      std::abort();
    }
    require(destination(*native) == 0U,
            "full-cache modified-immediate did not use fallback v0");
    require(live_ins == expected_live_ins,
            "full-cache modified-immediate imported the wrong state");
  };

  check("overwrite immediate",
        modified_immediate(false, false, 0x39U, 31U), 10U,
        code_base + 0x27'000U);
  check("combining immediate",
        modified_immediate(true, false, 0x39U, 31U), 11U,
        code_base + 0x28'000U);
}

void require_direct_two_register_operation(std::string_view name,
                                           std::uint32_t instruction,
                                           bool source_is_gpr,
                                           std::uint64_t pc) {
  const std::array guest{instruction};
  const auto compiled = jit_test::compile_block(pc, guest);
  require(compiled.guest_instructions == 1U,
          "JIT did not compile a direct two-register SIMD operation");
  const auto hot =
      std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

  std::optional<std::uint32_t> native;
  for (const auto word : hot) {
    if ((word & ~two_register_mask) !=
        (instruction & ~two_register_mask))
      continue;
    if (native.has_value()) {
      std::fprintf(stderr, "%.*s was emitted more than once\n",
                   static_cast<int>(name.size()), name.data());
      std::abort();
    }
    native = word;
  }
  if (!native.has_value()) {
    std::fprintf(stderr, "%.*s was not emitted directly\n",
                 static_cast<int>(name.size()), name.data());
    std::abort();
  }

  const auto native_destination = destination(*native);
  const auto native_source = left(*native);
  require(native_destination >= first_cached_vector &&
              native_destination <= last_cached_vector,
          "direct SIMD destination escaped the resident vector cache");
  if (source_is_gpr) {
    require(is_cached_gpr(native_source),
            "direct SIMD source escaped the resident GPR cache");
    return;
  }
  require(native_source >= first_cached_vector &&
              native_source <= last_cached_vector,
          "direct SIMD source escaped the resident vector cache");
  require((native_source == native_destination) ==
              (left(instruction) == destination(instruction)),
          "direct SIMD source/destination alias was not preserved");
}

void lane_reduce_and_shift_families_name_resident_registers_directly() {
  /*
   * Every case is a single native instruction. Alias cases additionally
   * prove that source allocation happens before destination definition.
   */
  require_direct_two_register_operation(
      "DUP (general)", 0x4e08'0c20U, true, code_base + 0xa000U);
  require_direct_two_register_operation(
      "INS (general)", 0x4e08'1c20U, true, code_base + 0xb000U);
  require_direct_two_register_operation(
      "INS (element)", 0x6e08'4420U, false, code_base + 0xc000U);
  require_direct_two_register_operation(
      "aliased INS (element)", 0x6e08'44a5U, false,
      code_base + 0xd000U);
  require_direct_two_register_operation(
      "aliased DUP (element)", 0x4e04'04c6U, false,
      code_base + 0xe000U);
  require_direct_two_register_operation(
      "aliased CNT", 0x4e20'5929U, false, code_base + 0xf000U);
  require_direct_two_register_operation(
      "aliased CMEQ (zero)", 0x4e20'9929U, false,
      code_base + 0x10'000U);
  require_direct_two_register_operation(
      "aliased ADDV", 0x4e31'b94aU, false, code_base + 0x11'000U);
  require_direct_two_register_operation(
      "aliased scalar ADDP", 0x5ef1'b96bU, false,
      code_base + 0x12'000U);
  require_direct_two_register_operation(
      "aliased SSRA", 0x4f38'158cU, false, code_base + 0x13'000U);
  require_direct_two_register_operation(
      "SHRN2", 0x4f18'842eU, false, code_base + 0x14'000U);
  require_direct_two_register_operation(
      "aliased SSHLL2", 0x4f0c'a5efU, false,
      code_base + 0x15'000U);
}

void narrowing_move_fallback_alias_is_preserved() {
  /*
   * Bind all eighteen resident vector slots, then use an unbound aliased
   * V31 source/destination. The fallback path must keep the native input and
   * output registers distinct; XTN2 additionally needs a second import for
   * the old lower destination half.
   */
  constexpr auto &fillers = full_vector_cache_fillers;
  const auto check = [&](bool upper, std::size_t expected_live_ins,
                         std::uint64_t pc) {
    std::array<std::uint32_t, fillers.size() + 1U> guest{};
    std::copy(fillers.begin(), fillers.end(), guest.begin());
    const auto target = 0x0e21'2800U |
                        (upper ? 0x4000'0000U : 0U) |
                        (31U << 5U) | 31U;
    guest.back() = target;

    const auto compiled = jit_test::compile_block(pc, guest);
    require(compiled.guest_instructions == guest.size(),
            "JIT did not compile the full-cache narrowing sequence");
    const auto hot =
        std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

    std::optional<std::uint32_t> native_target;
    std::size_t live_ins{};
    for (const auto word : hot) {
      live_ins += is_cpu_state_vector_load(word);
      if ((word & ~0x3ffU) == (target & ~0x3ffU))
        native_target = word;
    }
    require(native_target.has_value(),
            "JIT did not emit the fallback narrowing move");
    require(left(*native_target) != destination(*native_target),
            "fallback narrowing move collapsed aliased input and output");
    require(live_ins == expected_live_ins,
            "fallback narrowing move imported the wrong vector state");
  };

  check(false, 11U, code_base + 0x8000U);
  check(true, 12U, code_base + 0x9000U);
}

void lane_and_shift_fallback_aliases_are_preserved() {
  constexpr auto &fillers = full_vector_cache_fillers;
  const auto check = [&](std::string_view name, std::uint32_t target,
                         std::size_t expected_live_ins, std::uint64_t pc) {
    std::array<std::uint32_t, fillers.size() + 1U> guest{};
    std::copy(fillers.begin(), fillers.end(), guest.begin());
    guest.back() = target;

    const auto compiled = jit_test::compile_block(pc, guest);
    require(compiled.guest_instructions == guest.size(),
            "JIT did not compile the full-cache lane/shift sequence");
    const auto hot =
        std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);

    std::optional<std::uint32_t> native_target;
    std::size_t live_ins{};
    for (const auto word : hot) {
      live_ins += is_cpu_state_vector_load(word);
      if ((word & ~two_register_mask) ==
          (target & ~two_register_mask))
        native_target = word;
    }
    if (!native_target.has_value()) {
      std::fprintf(stderr, "%.*s fallback was not emitted directly\n",
                   static_cast<int>(name.size()), name.data());
      std::abort();
    }
    require(left(*native_target) == 1U &&
                destination(*native_target) == 0U,
            "full-cache SIMD alias did not use v1 input and v0 output");
    require(live_ins == expected_live_ins,
            "full-cache SIMD alias imported the wrong vector state");
  };

  check("aliased DUP (element)", 0x4e04'07ffU, 11U,
        code_base + 0x16'000U);
  check("aliased INS (element)", 0x6e08'47ffU, 12U,
        code_base + 0x17'000U);
  check("aliased SSRA", 0x4f38'17ffU, 12U,
        code_base + 0x18'000U);
}

void remaining_floating_fallback_aliases_are_preserved() {
  /*
   * Occupy all eighteen resident vector slots, then use V31 as every operand.
   * A zeroing destination must not overwrite the fallback source before the
   * native operation reads it.
   */
  constexpr auto &fillers = full_vector_cache_fillers;
  const auto check = [&](std::string_view name, std::uint32_t operation,
                         std::uint64_t pc) {
    std::array<std::uint32_t, fillers.size() + 1U> guest{};
    std::copy(fillers.begin(), fillers.end(), guest.begin());
    const auto target =
        operation | (31U << 16U) | (31U << 5U) | 31U;
    guest.back() = target;

    const auto compiled = jit_test::compile_block(pc, guest);
    require(compiled.guest_instructions == guest.size(),
            "JIT did not compile a full-cache floating alias sequence");
    const auto hot =
        std::span<const std::uint32_t>{compiled.words}.first(compiled.hot_words);
    std::optional<std::uint32_t> native_target;
    for (const auto word : hot)
      if ((word & ~register_mask) == (target & ~register_mask))
        native_target = word;
    if (!native_target.has_value()) {
      std::fprintf(stderr, "%.*s is missing its fallback native operation\n",
                   static_cast<int>(name.size()), name.data());
      std::abort();
    }
    require(left(*native_target) == right(*native_target),
            "aliased floating fallback sources became distinct values");
    require(destination(*native_target) != left(*native_target),
            "floating fallback destination clobbered its aliased source");
  };

  check("vector horizontal pairwise add", 0x6e20'd400U,
        code_base + 0x22'000U);
  check("scalar reciprocal step", 0x5e20'fc00U,
        code_base + 0x23'000U);
}

void remaining_simd_arithmetic_full_cache_fallbacks_are_exact() {
  constexpr auto &fillers = full_vector_cache_fillers;
  const auto compile = [&](std::uint32_t target, std::uint64_t pc) {
    std::array<std::uint32_t, fillers.size() + 1U> guest{};
    std::copy(fillers.begin(), fillers.end(), guest.begin());
    guest.back() = target;
    const auto compiled = jit_test::compile_block(pc, guest);
    require(compiled.guest_instructions == guest.size(),
            "JIT did not compile a full-cache SIMD sequence");
    return compiled;
  };
  const auto check_unrestricted =
      [&](std::string_view name, std::uint32_t operation,
          std::size_t expected_live_ins, std::uint64_t pc) {
        const auto target =
            operation | (31U << 16U) | (31U << 5U) | 31U;
        const auto compiled = compile(target, pc);
        const auto hot = std::span<const std::uint32_t>{compiled.words}
                             .first(compiled.hot_words);
        std::optional<std::uint32_t> native;
        std::size_t live_ins{};
        for (const auto word : hot) {
          live_ins += is_cpu_state_vector_load(word);
          if ((word & ~register_mask) == (target & ~register_mask))
            native = word;
        }
        if (!native.has_value()) {
          std::fprintf(stderr,
                       "%.*s is missing its full-cache native operation\n",
                       static_cast<int>(name.size()), name.data());
          std::abort();
        }
        require(left(*native) == 0U && right(*native) == 0U &&
                    destination(*native) == 2U,
                "full-cache unrestricted SIMD fallback used wrong registers");
        require(live_ins == expected_live_ins,
                "full-cache unrestricted SIMD fallback imported wrong state");
      };
  const auto check_restricted =
      [&](std::string_view name, std::uint32_t operation,
          std::uint64_t pc) {
        constexpr std::uint32_t restricted_register_mask =
            (15U << 16U) | (31U << 5U) | 31U;
        const auto target =
            operation | (15U << 16U) | (31U << 5U) | 31U;
        const auto compiled = compile(target, pc);
        const auto hot = std::span<const std::uint32_t>{compiled.words}
                             .first(compiled.hot_words);
        std::optional<std::uint32_t> native;
        std::size_t live_ins{};
        for (const auto word : hot) {
          live_ins += is_cpu_state_vector_load(word);
          if ((word & ~restricted_register_mask) ==
              (target & ~restricted_register_mask))
            native = word;
        }
        if (!native.has_value()) {
          std::fprintf(stderr,
                       "%.*s is missing its restricted fallback operation\n",
                       static_cast<int>(name.size()), name.data());
          std::abort();
        }
        require(left(*native) == 1U && right(*native) == 6U &&
                    destination(*native) == 0U,
                "full-cache pinned SIMD operation used wrong registers");
        require(live_ins == 12U,
                "full-cache pinned SIMD operation imported wrong state");
      };
  const auto check_table =
      [&](std::string_view name, std::uint32_t operation,
          std::size_t expected_live_ins, unsigned native_first,
          unsigned native_index, unsigned native_destination,
          std::uint64_t pc) {
        const auto target =
            operation | (31U << 16U) | (31U << 5U) | 31U;
        const auto compiled = compile(target, pc);
        const auto hot = std::span<const std::uint32_t>{compiled.words}
                             .first(compiled.hot_words);
        std::optional<std::uint32_t> native;
        std::size_t live_ins{};
        for (const auto word : hot) {
          live_ins += is_cpu_state_vector_load(word);
          if ((word & ~register_mask) == (target & ~register_mask))
            native = word;
        }
        if (!native.has_value()) {
          std::fprintf(stderr,
                       "%.*s is missing its full-cache table operation\n",
                       static_cast<int>(name.size()), name.data());
          std::abort();
        }
        require(left(*native) == native_first &&
                    right(*native) == native_index &&
                    destination(*native) == native_destination,
                "full-cache table fallback used wrong registers");
        require(live_ins == expected_live_ins,
                "full-cache table fallback imported wrong state");
      };

  const auto &host = uml::backend::host_arm_features();
  if (!host.advanced_simd)
    return;
  check_unrestricted(
      "widening accumulator", 0x0e20'5000U, 12U,
      code_base + 0x3e'000U);
  check_unrestricted(
      "vector permute", 0x4e00'2800U, 11U,
      code_base + 0x3f'000U);
  check_restricted(
      "halfword indexed integer accumulator", 0x6f40'0000U,
      code_base + 0x40'000U);
  check_table(
      "single-table TBL", 0x4e00'0000U, 12U, 0U, 4U, 5U,
      code_base + 0x41'000U);
  check_table(
      "single-table TBX", 0x4e00'1000U, 13U, 0U, 4U, 5U,
      code_base + 0x42'000U);
  check_table(
      "two-table TBL", 0x4e00'2000U, 11U, 30U, 30U, 30U,
      code_base + 0x43'000U);

  if (host.fp16 && host.floating_point)
    check_restricted(
        "half indexed floating accumulator", 0x4f00'1000U,
        code_base + 0x44'000U);
  if (host.has(uml::backend::ArmFeature::fhm) && host.afp)
    check_restricted(
        "half indexed fused-long accumulator", 0x4f80'0000U,
        code_base + 0x45'000U);
  if (host.has(uml::backend::ArmFeature::bf16))
    check_restricted(
        "bfloat indexed multiply-long accumulator", 0x4fc0'f000U,
        code_base + 0x46'000U);
  if (host.has(uml::backend::ArmFeature::lut)) {
    check_table(
        "single-table LUT", 0x4e40'2000U, 12U, 0U, 2U, 3U,
        code_base + 0x47'000U);
    check_table(
        "two-table LUT", 0x4e40'1000U, 11U, 30U, 30U, 30U,
        code_base + 0x48'000U);
  }
}

void destination_live_ins_are_translation_time_exact() {
  /*
   * Rd, Rn and Rm are deliberately distinct. These checks count generated
   * CpuState imports, so a false reads_old decision is visible even though it
   * does not change architectural results.
   */
  constexpr unsigned destination = 0U;
  constexpr unsigned first = 1U;
  constexpr unsigned second = 2U;
  constexpr auto fields = (second << 16U) | (first << 5U) | destination;
  constexpr auto unary_fields = (first << 5U) | destination;

  require_vector_live_ins(
      "scalar FADD",
      0x1e20'0800U | (2U << 12U) | fields, 2U);
  require_vector_live_ins(
      "vector FSQRT", 0x6ea1'f800U | unary_fields, 1U);
  require_vector_live_ins(
      "vector FRINTN", 0x4e21'8800U | unary_fields, 1U);
  require_vector_live_ins(
      "vector FABS", 0x4ea0'f800U | unary_fields, 1U);

  require_vector_live_ins(
      "floating compare with register",
      0x1e20'2000U | fields, 2U);
  require_vector_live_ins(
      "floating compare with zero",
      0x1e20'2008U | unary_fields, 1U);

  /* FCVTL2 selects the upper source half but does not preserve Rd. */
  require_vector_live_ins(
      "upper widening conversion", 0x4e21'7800U | unary_fields, 1U);
  /* FCVTN2 writes the upper destination half and preserves the lower half. */
  require_vector_live_ins(
      "upper narrowing conversion", 0x4e21'6800U | unary_fields, 2U);
  /* An aliased conversion must import Rn before the allocator defines Rd. */
  require_vector_live_ins(
      "aliased scalar integer conversion", 0x5e79'd800U, 1U);

  /* XTN overwrites Rd; XTN2 alone preserves the lower destination half. */
  require_vector_live_ins(
      "lower narrowing move", 0x0e21'2800U | unary_fields, 1U);
  require_vector_live_ins(
      "upper narrowing move", 0x4e21'2800U | unary_fields, 2U);
  /* The lower zeroing form still needs its aliased source imported. */
  require_vector_live_ins(
      "aliased lower narrowing move",
      0x0e21'2800U | (first << 5U) | first, 1U);
  require_vector_live_ins(
      "aliased upper narrowing move",
      0x4e21'2800U | (first << 5U) | first, 1U);

  require_vector_live_ins(
      "vector reciprocal estimate", 0x4ea1'd800U | unary_fields, 1U);
  require_vector_live_ins(
      "scalar reciprocal step", 0x5e20'fc00U | fields, 2U);

  const auto scalar_destination_live_ins =
      std::size_t{1U} + (uml::backend::host_arm_features().afp ? 1U : 0U);
  require_vector_live_ins(
      "scalar FSQRT", 0x1e21'c000U | unary_fields,
      scalar_destination_live_ins);
  require_vector_live_ins(
      "scalar FRINTN", 0x1e24'4000U | unary_fields,
      scalar_destination_live_ins);
  require_vector_live_ins(
      "scalar FABS", 0x1e20'c000U | unary_fields,
      scalar_destination_live_ins);
  require_vector_live_ins(
      "scalar precision conversion",
      0x1e22'4000U | (1U << 15U) | unary_fields,
      scalar_destination_live_ins);
  require_vector_live_ins(
      "scalar reciprocal estimate", 0x5ea1'd800U | unary_fields,
      scalar_destination_live_ins);

  require_vector_live_ins(
      "vector horizontal binary", 0x6e20'd400U | fields, 2U);
  require_vector_live_ins(
      "scalar horizontal pairwise", 0x7e70'd800U | unary_fields, 1U);
  require_vector_live_ins(
      "vector horizontal reduction", 0x6e30'f800U | unary_fields, 1U);

  const auto &host = uml::backend::host_arm_features();
  if (host.has(uml::backend::ArmFeature::fp8)) {
    require_vector_live_ins(
        "eight-bit convert up", 0x2e21'7800U | unary_fields, 1U);
    require_vector_live_ins(
        "eight-bit convert down lower", 0x0e40'f400U | fields, 2U);
    require_vector_live_ins(
        "eight-bit convert down upper", 0x4e00'f400U | fields, 3U);
  }
  if (host.has(uml::backend::ArmFeature::fp8dot4))
    require_vector_live_ins(
        "eight-bit dot accumulator", 0x0e00'fc00U | fields, 3U);
  if (host.has(uml::backend::ArmFeature::f8f32mm))
    require_vector_live_ins(
        "eight-bit matrix accumulator", 0x6e80'ec00U | fields, 3U);
}

void lane_reduce_and_shift_live_ins_are_exact() {
  require_vector_live_ins(
      "DUP (general)", 0x4e08'0c20U, 0U);
  require_vector_live_ins(
      "INS (general)", 0x4e08'1c20U, 1U);
  require_vector_live_ins(
      "INS (element)", 0x6e08'4420U, 2U);
  require_vector_live_ins(
      "aliased INS (element)", 0x6e08'44a5U, 1U);
  require_vector_live_ins(
      "aliased DUP (element)", 0x4e04'04c6U, 1U);
  require_vector_live_ins(
      "aliased CNT", 0x4e20'5929U, 1U);
  require_vector_live_ins(
      "aliased CMEQ (zero)", 0x4e20'9929U, 1U);
  require_vector_live_ins(
      "aliased ADDV", 0x4e31'b94aU, 1U);
  require_vector_live_ins(
      "aliased scalar ADDP", 0x5ef1'b96bU, 1U);
  require_vector_live_ins(
      "SSRA", 0x4f38'1420U, 2U);
  require_vector_live_ins(
      "aliased SSRA", 0x4f38'158cU, 1U);
  require_vector_live_ins(
      "SHL", 0x4f28'5420U, 1U);
  require_vector_live_ins(
      "SHRN2", 0x4f18'8420U, 2U);
  require_vector_live_ins(
      "aliased SHRN2", 0x4f18'84a5U, 1U);
  require_vector_live_ins(
      "aliased SSHLL2", 0x4f0c'a5efU, 1U);
}

void remaining_simd_arithmetic_live_ins_are_exact() {
  constexpr unsigned destination = 0U;
  constexpr unsigned left_source = 1U;
  constexpr unsigned right_source = 2U;
  constexpr auto fields =
      (right_source << 16U) | (left_source << 5U) | destination;
  const auto &host = uml::backend::host_arm_features();
  if (!host.advanced_simd)
    return;

  require_vector_live_ins(
      "widening SABAL accumulator", 0x0e20'5000U | fields, 3U);
  require_vector_live_ins(
      "widening SADDL overwrite", 0x0e20'0000U | fields, 2U);

  require_vector_live_ins(
      "word indexed MLA accumulator", 0x6f80'0000U | fields, 3U);
  require_vector_live_ins(
      "word indexed MUL overwrite", 0x4f80'8000U | fields, 2U);
  require_vector_live_ins(
      "halfword indexed MLA accumulator", 0x6f40'0000U | fields, 3U);
  require_vector_live_ins(
      "halfword indexed MUL overwrite", 0x4f40'8000U | fields, 2U);

  require_vector_live_ins(
      "TBL single-table overwrite", 0x4e00'0000U | fields, 2U);
  require_vector_live_ins(
      "TBX single-table accumulator", 0x4e00'1000U | fields, 3U);
  require_vector_live_ins(
      "TBL two-table overwrite",
      0x4e00'2000U | (3U << 16U) | (1U << 5U), 3U);
  require_vector_live_ins(
      "TRN1 overwrite", 0x4e00'2800U | fields, 2U);

  if (host.floating_point) {
    require_vector_live_ins(
        "word indexed FMLA accumulator", 0x4f80'1000U | fields, 3U);
    require_vector_live_ins(
        "word indexed FMUL overwrite", 0x4f80'9000U | fields, 2U);
  }
  if (host.fp16 && host.floating_point) {
    require_vector_live_ins(
        "half indexed FMLA accumulator", 0x4f00'1000U | fields, 3U);
    require_vector_live_ins(
        "half indexed FMUL overwrite", 0x4f00'9000U | fields, 2U);
  }

  if (host.has(uml::backend::ArmFeature::bf16)) {
    require_vector_live_ins(
        "bfloat vector dot accumulator", 0x6e40'fc00U | fields, 3U);
    require_vector_live_ins(
        "bfloat indexed dot accumulator", 0x4f40'f000U | fields, 3U);
    require_vector_live_ins(
        "bfloat vector multiply-long accumulator",
        0x6ec0'fc00U | fields, 3U);
    require_vector_live_ins(
        "bfloat indexed multiply-long accumulator",
        0x4fc0'f000U | fields, 3U);
    require_vector_live_ins(
        "bfloat matrix accumulator", 0x6e40'ec00U | fields, 3U);
  }
  if (host.has(uml::backend::ArmFeature::f16f32dot) && host.afp) {
    require_vector_live_ins(
        "half vector dot accumulator", 0x4e80'fc00U | fields, 3U);
    require_vector_live_ins(
        "half indexed dot accumulator", 0x4f40'9000U | fields, 3U);
  }
  if (host.has(uml::backend::ArmFeature::fhm) && host.afp) {
    require_vector_live_ins(
        "half vector fused-long accumulator",
        0x4e20'ec00U | fields, 3U);
    require_vector_live_ins(
        "half indexed fused-long accumulator",
        0x4f80'0000U | fields, 3U);
  }
  if (host.has(uml::backend::ArmFeature::f16f32mm) && host.afp)
    require_vector_live_ins(
        "half widening matrix accumulator",
        0x4e40'ec00U | fields, 3U);
  if (host.has(uml::backend::ArmFeature::dotprod)) {
    require_vector_live_ins(
        "integer vector dot accumulator", 0x4e80'9400U | fields, 3U);
    require_vector_live_ins(
        "integer indexed dot accumulator", 0x4f80'e000U | fields, 3U);
  }
  if (host.has(uml::backend::ArmFeature::i8mm))
    require_vector_live_ins(
        "integer matrix accumulator", 0x4e80'a400U | fields, 3U);
  if (host.has(uml::backend::ArmFeature::lut)) {
    require_vector_live_ins(
        "single-table LUT overwrite", 0x4e40'2000U | fields, 2U);
    require_vector_live_ins(
        "two-table LUT overwrite",
        0x4e40'1000U | (3U << 16U) | (1U << 5U), 3U);
  }
}

} // namespace

int main() {
  vector_dependency_chain_is_direct_and_resident();
  specialized_vector_dependency_chain_is_direct_and_resident();
  remaining_floating_families_name_resident_vectors_directly();
  remaining_simd_arithmetic_families_name_resident_vectors_directly();
  restricted_element_families_use_the_low_resident_bank();
  constrained_frequency_selects_two_stable_pins();
  constrained_pin_is_visible_to_an_earlier_producer();
  third_constrained_identity_uses_q2_when_high_bank_is_full();
  constrained_planning_stops_at_a_terminal();
  future_constrained_word_does_not_expand_a_terminal_prefix();
  unrestricted_fp8_binding_order_is_unchanged();
  unrestricted_low_pressure_shape_is_unchanged();
  ordinary_pressure_uses_both_unbound_low_slots();
  one_constrained_pin_leaves_one_low_slot_for_ordinary_pressure();
  two_constrained_pins_leave_no_low_slot_for_ordinary_pressure();
  ordinary_memory_names_resident_vector_registers_directly();
  literal_load_names_its_resident_vector_directly();
  atomic_floating_names_resident_vectors_directly();
  modified_immediates_name_resident_destination_directly();
  modified_immediate_full_cache_fallback_is_exact();
  narrowing_move_fallback_alias_is_preserved();
  lane_and_shift_fallback_aliases_are_preserved();
  remaining_floating_fallback_aliases_are_preserved();
  remaining_simd_arithmetic_full_cache_fallbacks_are_exact();
  destination_live_ins_are_translation_time_exact();
  lane_reduce_and_shift_families_name_resident_registers_directly();
  lane_reduce_and_shift_live_ins_are_exact();
  remaining_simd_arithmetic_live_ins_are_exact();
  return 0;
}
