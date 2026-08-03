// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fixed_transform_test_support.hpp"

namespace test = arm64_simd_fixed_transform_test;

namespace {

inline constexpr std::array<std::array<std::uint32_t, 4>, 5> edge_words{
    std::array<std::uint32_t, 4>{0, 0, 0, 0},
    std::array<std::uint32_t, 4>{0xffff'ffffU, 0xffff'ffffU, 0xffff'ffffU,
                                 0xffff'ffffU},
    std::array<std::uint32_t, 4>{0xaaaa'aaaaU, 0x5555'5555U, 0xaaaa'aaaaU,
                                 0x5555'5555U},
    std::array<std::uint32_t, 4>{0x8000'0000U, 0x0000'0001U, 0x7fff'ffffU,
                                 0xffff'fffeU},
    std::array<std::uint32_t, 4>{0xffff'fff0U, 0x0000'0010U, 0xffff'0000U,
                                 0x0000'ffffU},
};

void three_register_semantics(test::engine::EngineHarness &harness) {
  for (unsigned operation = 0; operation < 2U; ++operation)
    for (unsigned alias = 0; alias < test::alias3.size(); ++alias)
      for (unsigned edge = 0; edge < edge_words.size(); ++edge) {
        const auto registers = test::alias3[alias];
        auto initial =
            test::state(0x1500U + operation * 101U + alias * 11U + edge);
        test::set_words32(initial.vectors[registers.destination],
                          edge_words[edge]);
        test::set_words32(initial.vectors[registers.first],
                          edge_words[(edge + 1U) % edge_words.size()]);
        test::set_words32(initial.vectors[registers.second],
                          edge_words[(edge + 2U) % edge_words.size()]);
        test::check_transform3_three(harness, "later-three-semantics",
                                     operation, registers, initial);
      }
}

void four_register_semantics(test::engine::EngineHarness &harness) {
  for (unsigned alias = 0; alias < test::alias4.size(); ++alias)
    for (unsigned edge = 0; edge < edge_words.size(); ++edge) {
      const auto registers = test::alias4[alias];
      auto initial = test::state(0x2500U + alias * 11U + edge);
      test::set_words32(initial.vectors[registers.destination],
                        edge_words[edge]);
      test::set_words32(initial.vectors[registers.first],
                        edge_words[(edge + 1U) % edge_words.size()]);
      test::set_words32(initial.vectors[registers.second],
                        edge_words[(edge + 2U) % edge_words.size()]);
      test::set_words32(initial.vectors[registers.third],
                        edge_words[(edge + 3U) % edge_words.size()]);
      test::check_transform3_four(harness, "later-four-semantics", registers,
                                  initial);
    }
}

void indexed_semantics(test::engine::EngineHarness &harness) {
  for (unsigned operation = 0; operation < 4U; ++operation)
    for (unsigned lane = 0; lane < 4U; ++lane)
      for (unsigned alias = 0; alias < test::alias3.size(); ++alias)
        for (unsigned edge = 0; edge < edge_words.size(); ++edge) {
          const auto registers = test::alias3[alias];
          auto initial = test::state(0x3500U + operation * 401U + lane * 71U +
                                     alias * 11U + edge);
          test::set_words32(initial.vectors[registers.destination],
                            edge_words[edge]);
          test::set_words32(initial.vectors[registers.first],
                            edge_words[(edge + 1U) % edge_words.size()]);
          test::set_words32(initial.vectors[registers.second],
                            edge_words[(edge + 2U) % edge_words.size()]);
          test::check_transform3_indexed(harness, "later-indexed-semantics",
                                         operation, lane, registers, initial);
        }
}

} // namespace

int main() {
  test::engine::EngineHarness harness;
  three_register_semantics(harness);
  four_register_semantics(harness);
  indexed_semantics(harness);
  test::require(harness.counts().interpreter == 525,
                "later feature-three semantics case count");
  harness.print_summary("arm64-simd-later-feature-three-behavior");
  return 0;
}
