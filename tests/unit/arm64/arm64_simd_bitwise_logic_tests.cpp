// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_bitwise_test_support.hpp"

#include <array>

namespace bitwise = arm64_simd_bitwise_test;
namespace simd_test = arm64_simd_test;

namespace {

constexpr std::array operations{
    bitwise::Operation::bit_and, bitwise::Operation::bit_clear,
    bitwise::Operation::bit_or, bitwise::Operation::or_not,
    bitwise::Operation::bit_xor,
};

void every_logic_register_and_arrangement() {
  bitwise::Harness harness;
  for (const bool q : {false, true})
    for (const auto operation : operations) {
      const auto initial = simd_test::state(
          1U + static_cast<unsigned>(operation) * 13U + q * 29U);
      for (unsigned destination = 0; destination < 32; ++destination)
        for (unsigned left = 0; left < 32; ++left)
          for (unsigned right = 0; right < 32; ++right) {
            harness.check("simd-bitwise-logic-register-matrix", q, operation,
                          {destination, left, right}, initial);
          }
    }
  harness.print_summary("arm64-simd-bitwise-logic");
}

} // namespace

int main() {
  every_logic_register_and_arrangement();
  return 0;
}
