// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fixed_transform_test_support.hpp"

namespace test = arm64_simd_fixed_transform_test;

namespace {

void three_register_allocations(test::engine::EngineHarness &harness) {
  for (unsigned operation = 0; operation < 3U; ++operation) {
    for (unsigned reg = 0; reg < 32U; ++reg) {
      test::check_transform64_three(
          harness, "fixed64-valid-destination", operation, {reg, 1, 2},
          test::state(0x1200U + operation * 97U + reg));
      test::check_transform64_three(
          harness, "fixed64-valid-first", operation, {3, reg, 4},
          test::state(0x2200U + operation * 97U + reg));
      test::check_transform64_three(
          harness, "fixed64-valid-second", operation, {5, 6, reg},
          test::state(0x3200U + operation * 97U + reg));
    }
    for (unsigned index = 0; index < test::alias3.size(); ++index)
      test::check_transform64_three(
          harness, "fixed64-valid-alias", operation, test::alias3[index],
          test::state(0x4200U + operation * 17U + index));
  }
}

void two_register_allocations(test::engine::EngineHarness &harness) {
  for (unsigned reg = 0; reg < 32U; ++reg) {
    test::check_transform64_two(harness, "fixed64-two-valid-destination",
                                {reg, 1}, test::state(0x5200U + reg));
    test::check_transform64_two(harness, "fixed64-two-valid-source", {2, reg},
                                test::state(0x6200U + reg));
  }
  for (unsigned index = 0; index < test::alias2.size(); ++index)
    test::check_transform64_two(harness, "fixed64-two-valid-alias",
                                test::alias2[index],
                                test::state(0x7200U + index));
}

} // namespace

int main() {
  test::engine::EngineHarness harness;
  three_register_allocations(harness);
  two_register_allocations(harness);
  test::require(harness.counts().interpreter == 369,
                "fixed64 allocation case count");
  harness.print_summary("arm64-simd-fixed64-allocation");
  return 0;
}
