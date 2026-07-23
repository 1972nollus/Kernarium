// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_matrix_test_support.hpp"

#include <array>

namespace test = arm64_simd_matrix_test;
namespace base = arm64_simd_test;

namespace {

struct Registers final { unsigned destination; unsigned left; unsigned right; };

void signedness_matrix_layout_wraparound_and_aliases()
{
	base::Harness harness;
	constexpr std::array registers{
		Registers{28U, 29U, 30U}, Registers{1U, 1U, 2U},
		Registers{2U, 1U, 2U}, Registers{1U, 2U, 2U},
		Registers{3U, 3U, 3U}};
	for (const auto mode : {test::Mode::signed_signed,
	     test::Mode::unsigned_unsigned, test::Mode::unsigned_signed})
		for (const auto regs : registers)
			for (unsigned pattern = 0; pattern < 4U; ++pattern) {
				auto initial = base::state(pattern * 31U +
					static_cast<unsigned>(mode) * 7U + regs.destination);
				initial.fpsr = 0x84U;
				if (pattern == 1U) {
					initial.vectors[regs.left].bytes.fill(std::byte{0x80});
					initial.vectors[regs.right].bytes.fill(std::byte{0xff});
				} else if (pattern == 2U) {
					initial.vectors[regs.left].bytes.fill(std::byte{0xff});
					initial.vectors[regs.right].bytes.fill(std::byte{0x7f});
					initial.vectors[regs.destination].bytes.fill(std::byte{0xff});
				} else if (pattern == 3U) {
					for (unsigned byte = 0; byte < 16U; ++byte) {
						initial.vectors[regs.left].bytes[byte] =
							static_cast<std::byte>(byte * 17U + 3U);
						initial.vectors[regs.right].bytes[byte] =
							static_cast<std::byte>(byte * 29U + 5U);
					}
				}
				const auto encoded = test::instruction(mode, regs.destination,
					regs.left, regs.right);
				test::require_oracle(encoded, mode, regs.destination, regs.left,
					regs.right, initial, harness);
			}
}

} // namespace

int main()
{
	signedness_matrix_layout_wraparound_and_aliases();
	return 0;
}
