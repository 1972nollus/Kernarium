// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_dot_test_support.hpp"

#include <array>

namespace test = arm64_simd_dot_test;
namespace base = arm64_simd_test;

namespace {

struct Registers final { unsigned destination; unsigned left; unsigned right; };

void every_signedness_width_wraparound_and_alias()
{
	base::Harness harness;
	constexpr std::array registers{
		Registers{28U, 29U, 30U}, Registers{1U, 1U, 2U},
		Registers{2U, 1U, 2U}, Registers{1U, 2U, 2U},
		Registers{3U, 3U, 3U}};
	for (const auto mode : {test::Mode::signed_signed,
	     test::Mode::unsigned_unsigned, test::Mode::unsigned_signed})
		for (const bool q : {false, true})
			for (const auto regs : registers)
				for (unsigned pattern = 0; pattern < 3U; ++pattern) {
					auto initial = base::state(pattern * 31U + q +
						static_cast<unsigned>(mode) * 7U + regs.destination);
					initial.fpsr = 0x84U;
					if (pattern == 1U) {
						initial.vectors[regs.left].bytes.fill(std::byte{0x80});
						initial.vectors[regs.right].bytes.fill(std::byte{0xff});
					} else if (pattern == 2U) {
						initial.vectors[regs.left].bytes.fill(std::byte{0xff});
						initial.vectors[regs.right].bytes.fill(std::byte{0x7f});
						initial.vectors[regs.destination].bytes.fill(std::byte{0xff});
					}
					const auto encoded = test::vector_instruction(mode, q, 2U,
						regs.destination, regs.left, regs.right);
					test::require_oracle(encoded, mode, q, false,
						regs.destination, regs.left, regs.right, 0U,
						initial, harness);
				}
}

} // namespace

int main()
{
	every_signedness_width_wraparound_and_alias();
	return 0;
}
