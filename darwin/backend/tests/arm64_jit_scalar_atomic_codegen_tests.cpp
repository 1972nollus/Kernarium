// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime/host_features.hpp"
#include "runtime/jit/jit_test_api.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>

using uml::backend::tests::require;

namespace {

namespace jit_test = uml::backend::jit::testing;

constexpr std::uint64_t code_base = 0x5c10'0000;
constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::uint32_t register_mask =
    (31U << 16U) | (31U << 5U) | 31U;

[[nodiscard]] constexpr std::uint32_t
atomic(unsigned size, bool acquire, bool release, unsigned operation,
       unsigned base, unsigned source, unsigned destination) noexcept {
  const bool swap = operation == 8U;
  return 0x3820'0000U | ((size & 3U) << 30U) |
         (static_cast<unsigned>(acquire) << 23U) |
         (static_cast<unsigned>(release) << 22U) |
         ((source & 31U) << 16U) |
         (static_cast<unsigned>(swap) << 15U) |
         ((swap ? 0U : operation & 7U) << 12U) |
         ((base & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
compare_swap(unsigned size, bool acquire, bool release, unsigned base,
             unsigned compare, unsigned replacement) noexcept {
  return 0x08a0'0000U | ((size & 3U) << 30U) |
         (static_cast<unsigned>(acquire) << 22U) |
         ((compare & 31U) << 16U) |
         (static_cast<unsigned>(release) << 15U) |
         (31U << 10U) | ((base & 31U) << 5U) |
         (replacement & 31U);
}

[[nodiscard]] std::span<const std::uint32_t>
hot(const jit_test::CompiledCode &compiled) {
  require(compiled.hot_words <= compiled.words.size());
  return {compiled.words.data(), compiled.hot_words};
}

void require_one_native_operation(std::uint32_t guest,
                                  std::uint64_t pc) {
  const std::array program{guest, system_call};
  const auto compiled = jit_test::compile_block(pc, program);
  if (compiled.guest_instructions != program.size()) {
    std::fprintf(
        stderr,
        "scalar atomic codegen guest=%#010x pc=%#llx "
        "guest-instructions=%zu hot-words=%zu total-words=%zu illegal=%u\n",
        guest, static_cast<unsigned long long>(pc),
        compiled.guest_instructions, compiled.hot_words,
        compiled.words.size(), static_cast<unsigned>(compiled.guest_illegal));
  }
  require(!compiled.guest_illegal);
  require(compiled.guest_instructions == program.size());
  require(compiled.link_target_safe);
  require((compiled.direct_memory_write_mask & 1U) != 0U);

  unsigned matches = 0;
  for (const auto word : hot(compiled)) {
    if ((word & ~register_mask) != (guest & ~register_mask))
      continue;
    require(((word >> 5U) & 31U) == 8U);
    ++matches;
  }
  require(matches == 1U);
}

void every_scalar_rmw_variant_is_native() {
  std::uint64_t pc = code_base;
  for (unsigned size = 0; size < 4U; ++size)
    for (unsigned ordering = 0; ordering < 4U; ++ordering)
      for (unsigned operation = 0; operation < 9U; ++operation) {
        require_one_native_operation(
            atomic(size, (ordering & 2U) != 0U,
                   (ordering & 1U) != 0U, operation, 5U, 6U, 7U),
            pc);
        pc += 0x1000U;
      }

  require_one_native_operation(
      atomic(3U, true, true, 0U, 31U, 9U, 9U), pc);
  require_one_native_operation(
      atomic(2U, true, false, 8U, 5U, 31U, 31U), pc + 0x1000U);
}

void every_scalar_compare_swap_variant_is_native() {
  std::uint64_t pc = code_base + 0x10'0000U;
  for (unsigned size = 0; size < 4U; ++size)
    for (unsigned ordering = 0; ordering < 4U; ++ordering) {
      require_one_native_operation(
          compare_swap(size, (ordering & 2U) != 0U,
                       (ordering & 1U) != 0U, 5U, 6U, 7U),
          pc);
      pc += 0x1000U;
    }

  require_one_native_operation(
      compare_swap(3U, true, true, 31U, 9U, 9U), pc);
  require_one_native_operation(
      compare_swap(2U, true, false, 5U, 31U, 31U), pc + 0x1000U);
}

void verifier_does_not_replay_concurrent_memory_operations() {
  for (unsigned size = 0; size < 4U; ++size)
    for (unsigned ordering = 0; ordering < 4U; ++ordering)
      for (unsigned operation = 0; operation < 9U; ++operation)
        require(!jit_test::verification_replay_safe_for_testing(
            atomic(size, (ordering & 2U) != 0U,
                   (ordering & 1U) != 0U, operation, 1U, 0U, 0U)));
  require(!jit_test::verification_replay_safe_for_testing(
      compare_swap(2U, true, false, 1U, 0U, 0U)));
  require(!jit_test::verification_replay_safe_for_testing(0xf940'0b90U));
  require(jit_test::verification_replay_safe_for_testing(0x8b00'0020U));
}

} // namespace

int main() {
  if (!uml::backend::host_arm_features().has(
          uml::backend::ArmFeature::lse)) {
    std::puts("arm64-jit-scalar-atomic-codegen: skipped (host feature)");
    return 0;
  }
  every_scalar_rmw_variant_is_native();
  every_scalar_compare_swap_variant_is_native();
  verifier_does_not_replay_concurrent_memory_operations();
  std::puts("arm64-jit-scalar-atomic-codegen: passed");
  return 0;
}
