// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fixed_transform_test_support.hpp"

namespace test = arm64_simd_fixed_transform_test;

namespace {

void valid_three_register_allocations(test::engine::EngineHarness &harness) {
  for (unsigned operation = 0; operation < 7U; ++operation) {
    for (unsigned reg = 0; reg < 32U; ++reg) {
      test::check_transform32_three(
          harness, "fixed32-valid-destination", operation, {reg, 1, 2},
          test::state(0x1000U + operation * 97U + reg));
      test::check_transform32_three(
          harness, "fixed32-valid-first", operation, {3, reg, 4},
          test::state(0x2000U + operation * 97U + reg));
      test::check_transform32_three(
          harness, "fixed32-valid-second", operation, {5, 6, reg},
          test::state(0x3000U + operation * 97U + reg));
    }
    for (unsigned index = 0; index < test::alias3.size(); ++index)
      test::check_transform32_three(
          harness, "fixed32-valid-alias", operation, test::alias3[index],
          test::state(0x4000U + operation * 17U + index));
  }
}

void valid_two_register_allocations(test::engine::EngineHarness &harness) {
  for (unsigned operation = 0; operation < 3U; ++operation) {
    for (unsigned reg = 0; reg < 32U; ++reg) {
      test::check_transform32_two(harness, "fixed32-two-valid-destination",
                                  operation, {reg, 1},
                                  test::state(0x5000U + operation * 97U + reg));
      test::check_transform32_two(harness, "fixed32-two-valid-source",
                                  operation, {2, reg},
                                  test::state(0x6000U + operation * 97U + reg));
    }
    for (unsigned index = 0; index < test::alias2.size(); ++index)
      test::check_transform32_two(
          harness, "fixed32-two-valid-alias", operation, test::alias2[index],
          test::state(0x7000U + operation * 17U + index));
  }
}

void reserved_allocations(test::engine::EngineHarness &harness) {
  for (unsigned reg = 0; reg < 32U; ++reg) {
    harness.check_illegal("fixed32-reserved-destination",
                          test::transform32_three(7, {reg, 1, 2}),
                          test::state(0x8000U + reg));
    harness.check_illegal("fixed32-reserved-first",
                          test::transform32_three(7, {3, reg, 4}),
                          test::state(0x8100U + reg));
    harness.check_illegal("fixed32-reserved-second",
                          test::transform32_three(7, {5, 6, reg}),
                          test::state(0x8200U + reg));
    harness.check_illegal("fixed32-two-reserved-destination",
                          test::transform32_two(3, {reg, 1}),
                          test::state(0x8300U + reg));
    harness.check_illegal("fixed32-two-reserved-source",
                          test::transform32_two(3, {2, reg}),
                          test::state(0x8400U + reg));
  }
  for (unsigned index = 0; index < test::alias3.size(); ++index)
    harness.check_illegal("fixed32-reserved-alias",
                          test::transform32_three(7, test::alias3[index]),
                          test::state(0x8500U + index));
  for (unsigned index = 0; index < test::alias2.size(); ++index)
    harness.check_illegal("fixed32-two-reserved-alias",
                          test::transform32_two(3, test::alias2[index]),
                          test::state(0x8600U + index));
}

} // namespace

int main() {
  test::engine::EngineHarness harness;
  valid_three_register_allocations(harness);
  valid_two_register_allocations(harness);
  reserved_allocations(harness);
  test::require(harness.counts().interpreter == 1072,
                "fixed32 allocation case count");
  harness.print_summary("arm64-simd-fixed32-allocation");
  return 0;
}
