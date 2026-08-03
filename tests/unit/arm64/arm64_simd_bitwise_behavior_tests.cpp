// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_bitwise_test_support.hpp"

#include <array>

namespace bitwise = arm64_simd_bitwise_test;
namespace simd_test = arm64_simd_test;

namespace {

constexpr std::array operations{
    bitwise::Operation::bit_and,      bitwise::Operation::bit_clear,
    bitwise::Operation::bit_or,       bitwise::Operation::or_not,
    bitwise::Operation::bit_xor,      bitwise::Operation::select,
    bitwise::Operation::insert_true,  bitwise::Operation::insert_false,
};

constexpr std::array aliases{
    bitwise::Registers{0, 1, 2}, bitwise::Registers{1, 1, 2},
    bitwise::Registers{2, 1, 2}, bitwise::Registers{0, 1, 1},
    bitwise::Registers{3, 3, 3}, bitwise::Registers{31, 30, 29},
};

[[nodiscard]] uml::cpu::arm64::CpuState known_answer_state() {
  auto initial = simd_test::state(83);
  for (unsigned byte = 0; byte < 16; ++byte) {
    initial.vectors[0].bytes[byte] =
        static_cast<std::byte>(0xa5U ^ (byte * 3U));
    initial.vectors[1].bytes[byte] =
        static_cast<std::byte>(0x3cU + byte * 7U);
    initial.vectors[2].bytes[byte] =
        static_cast<std::byte>(0xc3U - byte * 5U);
  }
  return initial;
}

void known_answers_and_aliases(bitwise::Harness &harness) {
  for (const bool q : {false, true})
    for (const auto operation : operations) {
      harness.check("simd-bitwise-known-answer", q, operation, {0, 1, 2},
                    known_answer_state());
      for (unsigned seed = 0; seed < 4; ++seed)
        for (const auto registers : aliases)
          harness.check("simd-bitwise-alias", q, operation, registers,
                        simd_test::state(seed * 37U +
                                         static_cast<unsigned>(operation)));
    }
}

void every_mov_alias(bitwise::Harness &harness) {
  for (const bool q : {false, true})
    for (unsigned destination = 0; destination < 32; ++destination)
      for (unsigned source = 0; source < 32; ++source)
        harness.check("simd-bitwise-mov-alias", q,
                      bitwise::Operation::bit_or,
                      {destination, source, source},
                      simd_test::state(47U + q * 71U));
}

} // namespace

int main() {
  bitwise::Harness harness;
  known_answers_and_aliases(harness);
  every_mov_alias(harness);
  harness.print_summary("arm64-simd-bitwise-behavior");
  return 0;
}
