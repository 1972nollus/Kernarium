// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fixed_transform_test_support.hpp"

namespace test = arm64_simd_fixed_transform_test;

namespace {

void first_group(test::engine::EngineHarness &harness) {
  for (unsigned operation = 0; operation < 7U; ++operation)
    for (unsigned alias = 0; alias < test::alias3.size(); ++alias)
      test::check_transform32_three(
          harness, "portable-fixed32-three", operation, test::alias3[alias],
          test::state(0x1700U + operation * 17U + alias));
  for (unsigned operation = 0; operation < 3U; ++operation)
    for (unsigned alias = 0; alias < test::alias2.size(); ++alias)
      test::check_transform32_two(
          harness, "portable-fixed32-two", operation, test::alias2[alias],
          test::state(0x2700U + operation * 17U + alias));
}

void wide_group(test::engine::EngineHarness &harness) {
  for (unsigned operation = 0; operation < 3U; ++operation)
    for (unsigned alias = 0; alias < test::alias3.size(); ++alias)
      test::check_transform64_three(
          harness, "portable-fixed64-three", operation, test::alias3[alias],
          test::state(0x3700U + operation * 17U + alias));
  for (unsigned alias = 0; alias < test::alias2.size(); ++alias)
    test::check_transform64_two(harness, "portable-fixed64-two",
                                test::alias2[alias],
                                test::state(0x4700U + alias));
}

void later_group(test::engine::EngineHarness &harness) {
  for (unsigned operation = 0; operation < 2U; ++operation)
    for (unsigned alias = 0; alias < test::alias3.size(); ++alias)
      test::check_transform3_three(
          harness, "portable-later-three", operation, test::alias3[alias],
          test::state(0x5700U + operation * 17U + alias));
  for (unsigned alias = 0; alias < test::alias4.size(); ++alias)
    test::check_transform3_four(harness, "portable-later-four",
                                test::alias4[alias],
                                test::state(0x6700U + alias));
  for (unsigned operation = 0; operation < 4U; ++operation)
    for (unsigned lane = 0; lane < 4U; ++lane)
      for (unsigned alias = 0; alias < test::alias3.size(); ++alias)
        test::check_transform3_indexed(
            harness, "portable-later-indexed", operation, lane,
            test::alias3[alias],
            test::state(0x7700U + operation * 97U + lane * 17U + alias));
  for (unsigned alias = 0; alias < test::alias3.size(); ++alias)
    test::check_transform4_three(harness, "portable-later-key",
                                 test::alias3[alias],
                                 test::state(0x8700U + alias));
  for (unsigned alias = 0; alias < test::alias2.size(); ++alias)
    test::check_transform4_two(harness, "portable-later-round",
                               test::alias2[alias],
                               test::state(0x9700U + alias));
}

} // namespace

int main() {
  test::engine::EngineHarnessOptions options{};
  options.force_software_fixed_transforms = true;
  test::engine::EngineHarness harness{options};
  first_group(harness);
  wide_group(harness);
  later_group(harness);
  test::require(harness.counts().interpreter == 170,
                "portable fixed transform case count");
  harness.print_summary("arm64-simd-fixed-transform-portable");
  return 0;
}
