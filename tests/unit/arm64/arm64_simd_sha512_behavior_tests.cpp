// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fixed_transform_test_support.hpp"

namespace test = arm64_simd_fixed_transform_test;

namespace {

inline constexpr std::array<std::array<std::uint64_t, 2>, 5> edge_words{
    std::array<std::uint64_t, 2>{0, 0},
    std::array<std::uint64_t, 2>{0xffff'ffff'ffff'ffffULL,
                                 0xffff'ffff'ffff'ffffULL},
    std::array<std::uint64_t, 2>{0xaaaa'aaaa'aaaa'aaaaULL,
                                 0x5555'5555'5555'5555ULL},
    std::array<std::uint64_t, 2>{0x8000'0000'0000'0000ULL,
                                 0x0000'0000'0000'0001ULL},
    std::array<std::uint64_t, 2>{0xffff'ffff'ffff'fff0ULL,
                                 0x0000'0000'0000'0010ULL},
};

void three_register_semantics(test::engine::EngineHarness &harness) {
  for (unsigned operation = 0; operation < 3U; ++operation)
    for (unsigned alias = 0; alias < test::alias3.size(); ++alias)
      for (unsigned edge = 0; edge < edge_words.size(); ++edge) {
        const auto registers = test::alias3[alias];
        auto initial =
            test::state(0x1300U + operation * 101U + alias * 11U + edge);
        test::set_words64(initial.vectors[registers.destination],
                          edge_words[edge]);
        test::set_words64(initial.vectors[registers.first],
                          edge_words[(edge + 1U) % edge_words.size()]);
        test::set_words64(initial.vectors[registers.second],
                          edge_words[(edge + 2U) % edge_words.size()]);
        test::check_transform64_three(harness, "fixed64-three-semantics",
                                      operation, registers, initial);
      }
}

void two_register_semantics(test::engine::EngineHarness &harness) {
  for (unsigned alias = 0; alias < test::alias2.size(); ++alias)
    for (unsigned edge = 0; edge < edge_words.size(); ++edge) {
      const auto registers = test::alias2[alias];
      auto initial = test::state(0x2300U + alias * 11U + edge);
      test::set_words64(initial.vectors[registers.destination],
                        edge_words[edge]);
      test::set_words64(initial.vectors[registers.source],
                        edge_words[(edge + 3U) % edge_words.size()]);
      test::check_transform64_two(harness, "fixed64-two-semantics", registers,
                                  initial);
    }
}

} // namespace

int main() {
  test::engine::EngineHarness harness;
  three_register_semantics(harness);
  two_register_semantics(harness);
  test::require(harness.counts().interpreter == 85,
                "fixed64 semantics case count");
  harness.print_summary("arm64-simd-fixed64-behavior");
  return 0;
}
