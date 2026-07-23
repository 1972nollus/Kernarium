// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_cvtfi_test_support.hpp"

namespace test = arm64_simd_cvtfi_test;
namespace base = arm64_simd_test;

int main()
{
	base::Harness harness;
	for (const bool unsigned_source : {false, true}) {
		auto overflow = base::state(unsigned_source);
		overflow.registers[29] = unsigned_source ?
			0xffff'ffff'ffff'ffffULL : 0x7fff'ffff'ffff'ffffULL;
		const std::array infinity{std::uint64_t{0x7c00U}};
		test::require_vector(test::gpr_instruction(unsigned_source, 64U,
			16U, 28U, 29U), 16U, 2U, true, 28U, infinity,
			(1U << 2) | (1U << 4), overflow, harness);

		overflow.fpcr = 1U << 26;
		const std::array maximum{std::uint64_t{0x7fffU}};
		test::require_vector(test::gpr_instruction(unsigned_source, 64U,
			16U, 28U, 29U), 16U, 2U, true, 28U, maximum,
			1U, overflow, harness);

		harness.require_illegal(test::gpr_instruction(unsigned_source,
			32U, 32U, 28U, 29U) | 0x0080'0000U, base::state(90U));
		harness.require_illegal(test::fpr_instruction(unsigned_source,
			32U, 32U, 28U, 29U), base::state(91U));
		const test::Shape short_double{64U, 8U, false, false};
		harness.require_illegal(test::simd_instruction(unsigned_source,
			short_double, 28U, 29U), base::state(92U));
		harness.require_illegal(test::gpr_fixed_instruction(unsigned_source,
			32U, 32U, 33U, 28U, 29U), base::state(93U));
		harness.require_illegal(test::simd_fixed_instruction(unsigned_source,
			short_double, 1U, 28U, 29U), base::state(94U));
		const auto invalid_immediate = (test::simd_fixed_instruction(
			unsigned_source, test::shapes[3], 16U, 28U, 29U) &
			~0x007f'0000U) | (15U << 16);
		harness.require_illegal(invalid_immediate, base::state(95U));
	}
	return 0;
}
