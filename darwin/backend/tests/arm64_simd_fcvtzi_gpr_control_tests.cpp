// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void nan_infinity_and_afp_controls()
{
	base::Harness harness;
	for (const bool unsigned_result : {false, true}) {
		auto nan = base::state(unsigned_result);
		test::set_gpr_source(nan, 29U, 64U, 0x7ff8'1234'5678'9abcULL);
		test::require_gpr(test::gpr_instruction(unsigned_result, 64U, 64U,
			28U, 29U), 64U, 28U, 0U, 1U, nan, harness);

		auto infinity = base::state(10U + unsigned_result);
		test::set_gpr_source(infinity, 29U, 64U,
			0x7ff0'0000'0000'0000ULL);
		test::require_gpr(test::gpr_instruction(unsigned_result, 64U, 64U,
			28U, 29U), 64U, 28U,
			unsigned_result ? ~std::uint64_t{0} : 0x7fff'ffff'ffff'ffffULL,
			1U, infinity, harness);
	}

	struct Entry final {
		std::uint32_t fpcr;
		std::uint32_t exceptions;
	};
	const Entry entries[]{
		{0U, 1U << 4}, {1U << 24, 1U << 7}, {1U, 0U},
		{2U, (1U << 7) | (1U << 4)}, {3U, 0U},
	};
	for (const auto entry : entries) {
		auto initial = base::state(entry.fpcr);
		initial.fpcr = entry.fpcr;
		initial.fpsr = 0x10U;
		test::set_gpr_source(initial, 29U, 32U, 1U);
		test::require_gpr(test::gpr_instruction(false, 32U, 32U,
			28U, 29U), 32U, 28U, 0U, entry.exceptions, initial, harness);
	}
}

void alternative_half_precision()
{
	base::Harness harness;
	for (const bool ahp : {false, true}) {
		auto initial = base::state(80U + ahp);
		initial.fpcr = ahp ? 1U << 26 : 0U;
		test::set_gpr_source(initial, 29U, 16U, 0x7c00U);
		test::require_gpr(test::gpr_instruction(false, 16U, 64U,
			28U, 29U), 64U, 28U,
			ahp ? 65536U : 0x7fff'ffff'ffff'ffffULL,
			ahp ? 0U : 1U, initial, harness);
	}
}

} // namespace

int main()
{
	nan_infinity_and_afp_controls();
	alternative_half_precision();
	return 0;
}
