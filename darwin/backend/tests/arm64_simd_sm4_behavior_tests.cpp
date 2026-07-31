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

void specification_examples(test::engine::EngineHarness &harness) {
  auto key_state = test::state(0x4500U);
  test::set_words32(key_state.vectors[1],
                    {0xa292'ffa1U, 0xdf01'febfU, 0x99a1'2b0fU, 0xc424'10ccU});
  test::set_words32(key_state.vectors[2],
                    {0x0007'0e15U, 0x1c23'2a31U, 0x383f'464dU, 0x545b'6269U});
  constexpr std::array expected_keys{0xf121'86f9U, 0x4166'2b61U, 0x5a6a'b19aU,
                                     0x7ba9'2077U};
  test::require(test::oracle_transform4_three(key_state, {0, 1, 2}) ==
                    expected_keys,
                "later key specification oracle");
  test::check_transform4_three(harness, "later-key-specification-example",
                               {0, 1, 2}, key_state);

  auto round_state = test::state(0x4600U);
  test::set_words32(round_state.vectors[0],
                    {0x0123'4567U, 0x89ab'cdefU, 0xfedc'ba98U, 0x7654'3210U});
  test::set_words32(round_state.vectors[1], expected_keys);
  constexpr std::array expected_rounds{0x27fa'd345U, 0xa18b'4cb2U, 0x11c1'e22aU,
                                       0xcc13'e2eeU};
  test::require(test::oracle_transform4_two(round_state, {0, 1}) ==
                    expected_rounds,
                "later round specification oracle");
  test::check_transform4_two(harness, "later-round-specification-example",
                             {0, 1}, round_state);
}

void three_register_semantics(test::engine::EngineHarness &harness) {
  for (unsigned alias = 0; alias < test::alias3.size(); ++alias)
    for (unsigned edge = 0; edge < edge_words.size(); ++edge) {
      const auto registers = test::alias3[alias];
      auto initial = test::state(0x5500U + alias * 11U + edge);
      test::set_words32(initial.vectors[registers.destination],
                        edge_words[edge]);
      test::set_words32(initial.vectors[registers.first],
                        edge_words[(edge + 1U) % edge_words.size()]);
      test::set_words32(initial.vectors[registers.second],
                        edge_words[(edge + 2U) % edge_words.size()]);
      test::check_transform4_three(harness, "later-key-semantics", registers,
                                   initial);
    }
}

void two_register_semantics(test::engine::EngineHarness &harness) {
  for (unsigned alias = 0; alias < test::alias2.size(); ++alias)
    for (unsigned edge = 0; edge < edge_words.size(); ++edge) {
      const auto registers = test::alias2[alias];
      auto initial = test::state(0x6500U + alias * 11U + edge);
      test::set_words32(initial.vectors[registers.destination],
                        edge_words[edge]);
      test::set_words32(initial.vectors[registers.source],
                        edge_words[(edge + 3U) % edge_words.size()]);
      test::check_transform4_two(harness, "later-round-semantics", registers,
                                 initial);
    }
}

} // namespace

int main() {
  test::engine::EngineHarness harness;
  specification_examples(harness);
  three_register_semantics(harness);
  two_register_semantics(harness);
  test::require(harness.counts().interpreter == 37,
                "later feature-four semantics case count");
  harness.print_summary("arm64-simd-later-feature-four-behavior");
  return 0;
}
