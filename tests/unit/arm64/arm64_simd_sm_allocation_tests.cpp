// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fixed_transform_test_support.hpp"

namespace test = arm64_simd_fixed_transform_test;

namespace {

void three_register_allocations(test::engine::EngineHarness &harness) {
  for (unsigned operation = 0; operation < 3U; ++operation) {
    for (unsigned reg = 0; reg < 32U; ++reg) {
      const auto initial = test::state(0x1400U + operation * 101U + reg);
      if (operation < 2U) {
        test::check_transform3_three(harness, "later-three-valid-destination",
                                     operation, {reg, 1, 2}, initial);
        test::check_transform3_three(harness, "later-three-valid-first",
                                     operation, {3, reg, 4}, initial);
        test::check_transform3_three(harness, "later-three-valid-second",
                                     operation, {5, 6, reg}, initial);
      } else {
        test::check_transform4_three(harness, "later-key-valid-destination",
                                     {reg, 1, 2}, initial);
        test::check_transform4_three(harness, "later-key-valid-first",
                                     {3, reg, 4}, initial);
        test::check_transform4_three(harness, "later-key-valid-second",
                                     {5, 6, reg}, initial);
      }
    }
    for (unsigned index = 0; index < test::alias3.size(); ++index) {
      const auto initial = test::state(0x2400U + operation * 17U + index);
      if (operation < 2U)
        test::check_transform3_three(harness, "later-three-valid-alias",
                                     operation, test::alias3[index], initial);
      else
        test::check_transform4_three(harness, "later-key-valid-alias",
                                     test::alias3[index], initial);
    }
  }
}

void four_register_allocations(test::engine::EngineHarness &harness) {
  for (unsigned reg = 0; reg < 32U; ++reg) {
    test::check_transform3_four(harness, "later-four-valid-destination",
                                {reg, 1, 2, 3}, test::state(0x3400U + reg));
    test::check_transform3_four(harness, "later-four-valid-first",
                                {4, reg, 5, 6}, test::state(0x3500U + reg));
    test::check_transform3_four(harness, "later-four-valid-second",
                                {7, 8, reg, 9}, test::state(0x3600U + reg));
    test::check_transform3_four(harness, "later-four-valid-third",
                                {10, 11, 12, reg}, test::state(0x3700U + reg));
  }
  for (unsigned index = 0; index < test::alias4.size(); ++index)
    test::check_transform3_four(harness, "later-four-valid-alias",
                                test::alias4[index],
                                test::state(0x3800U + index));
}

void indexed_allocations(test::engine::EngineHarness &harness) {
  for (unsigned operation = 0; operation < 4U; ++operation)
    for (unsigned lane = 0; lane < 4U; ++lane) {
      for (unsigned reg = 0; reg < 32U; ++reg) {
        const auto initial =
            test::state(0x4400U + operation * 401U + lane * 71U + reg);
        test::check_transform3_indexed(harness,
                                       "later-indexed-valid-destination",
                                       operation, lane, {reg, 1, 2}, initial);
        test::check_transform3_indexed(harness, "later-indexed-valid-first",
                                       operation, lane, {3, reg, 4}, initial);
        test::check_transform3_indexed(harness, "later-indexed-valid-second",
                                       operation, lane, {5, 6, reg}, initial);
      }
      for (unsigned index = 0; index < test::alias3.size(); ++index)
        test::check_transform3_indexed(
            harness, "later-indexed-valid-alias", operation, lane,
            test::alias3[index],
            test::state(0x5400U + operation * 97U + lane * 17U + index));
    }
}

void two_register_allocations(test::engine::EngineHarness &harness) {
  for (unsigned reg = 0; reg < 32U; ++reg) {
    test::check_transform4_two(harness, "later-round-valid-destination",
                               {reg, 1}, test::state(0x6400U + reg));
    test::check_transform4_two(harness, "later-round-valid-source", {2, reg},
                               test::state(0x6500U + reg));
  }
  for (unsigned index = 0; index < test::alias2.size(); ++index)
    test::check_transform4_two(harness, "later-round-valid-alias",
                               test::alias2[index],
                               test::state(0x6600U + index));
}

void reserved_three_register_allocations(test::engine::EngineHarness &harness) {
  const auto encode = [](test::Registers3 registers) {
    return 0xce60'cc00U | (registers.second << 16) | (registers.first << 5) |
           registers.destination;
  };
  for (unsigned reg = 0; reg < 32U; ++reg) {
    harness.check_illegal("later-three-reserved-destination",
                          encode({reg, 1, 2}), test::state(0x7400U + reg));
    harness.check_illegal("later-three-reserved-first", encode({3, reg, 4}),
                          test::state(0x7500U + reg));
    harness.check_illegal("later-three-reserved-second", encode({5, 6, reg}),
                          test::state(0x7600U + reg));
  }
  for (unsigned index = 0; index < test::alias3.size(); ++index)
    harness.check_illegal("later-three-reserved-alias",
                          encode(test::alias3[index]),
                          test::state(0x7700U + index));
}

void reserved_four_register_allocations(test::engine::EngineHarness &harness) {
  const auto encode = [](test::Registers4 registers) {
    return 0xce60'0000U | (registers.second << 16) | (registers.third << 10) |
           (registers.first << 5) | registers.destination;
  };
  for (unsigned reg = 0; reg < 32U; ++reg) {
    harness.check_illegal("later-four-reserved-destination",
                          encode({reg, 1, 2, 3}), test::state(0x7800U + reg));
    harness.check_illegal("later-four-reserved-first", encode({4, reg, 5, 6}),
                          test::state(0x7900U + reg));
    harness.check_illegal("later-four-reserved-second", encode({7, 8, reg, 9}),
                          test::state(0x7a00U + reg));
    harness.check_illegal("later-four-reserved-third",
                          encode({10, 11, 12, reg}),
                          test::state(0x7b00U + reg));
  }
  for (unsigned index = 0; index < test::alias4.size(); ++index)
    harness.check_illegal("later-four-reserved-alias",
                          encode(test::alias4[index]),
                          test::state(0x7c00U + index));
}

void reserved_two_register_allocations(test::engine::EngineHarness &harness) {
  for (const auto base : {0xcec0'8800U, 0xcec0'8c00U}) {
    const auto encode = [base](test::Registers2 registers) {
      return base | (registers.source << 5) | registers.destination;
    };
    for (unsigned reg = 0; reg < 32U; ++reg) {
      harness.check_illegal("later-two-reserved-destination", encode({reg, 1}),
                            test::state(base + reg));
      harness.check_illegal("later-two-reserved-source", encode({2, reg}),
                            test::state(base + 0x100U + reg));
    }
    for (unsigned index = 0; index < test::alias2.size(); ++index)
      harness.check_illegal("later-two-reserved-alias",
                            encode(test::alias2[index]),
                            test::state(base + 0x200U + index));
  }
}

} // namespace

int main() {
  test::engine::EngineHarness harness;
  three_register_allocations(harness);
  four_register_allocations(harness);
  indexed_allocations(harness);
  two_register_allocations(harness);
  reserved_three_register_allocations(harness);
  reserved_four_register_allocations(harness);
  reserved_two_register_allocations(harness);
  test::require(harness.counts().interpreter == 2504,
                "later allocation case count");
  harness.print_summary("arm64-simd-later-transform-allocation");
  return 0;
}
