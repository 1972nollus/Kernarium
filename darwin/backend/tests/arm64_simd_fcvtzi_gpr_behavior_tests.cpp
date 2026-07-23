// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void truncation_sign_and_w_zero_extension()
{
	base::Harness harness;
	for (const unsigned width : {16U, 32U, 64U}) {
		auto positive = base::state(width);
		const auto positive_bits = width == 16U ? 0x3e00U :
			width == 32U ? 0x3fc0'0000U : 0x3ff8'0000'0000'0000ULL;
		test::set_gpr_source(positive, 29U, width, positive_bits);
		test::require_gpr(test::gpr_instruction(false, width, 32U,
			28U, 29U), 32U, 28U, 1U, 1U << 4, positive, harness);

		auto negative = base::state(width + 10U);
		test::set_gpr_source(negative, 29U, width,
			positive_bits | (std::uint64_t{1} << (width - 1U)));
		test::require_gpr(test::gpr_instruction(false, width, 64U,
			28U, 29U), 64U, 28U, ~std::uint64_t{0}, 1U << 4,
			negative, harness);

		auto unsigned_fraction = negative;
		const auto half = width == 16U ? 0xb800U : width == 32U ?
			0xbf00'0000U : 0xbfe0'0000'0000'0000ULL;
		test::set_gpr_source(unsigned_fraction, 29U, width, half);
		test::require_gpr(test::gpr_instruction(true, width, 64U,
			28U, 29U), 64U, 28U, 0U, 1U << 4,
			unsigned_fraction, harness);
	}
}

void saturation_and_invalid_rules()
{
	base::Harness harness;
	auto positive = base::state(40U);
	test::set_gpr_source(positive, 29U, 32U, 0x4f00'0000U);
	test::require_gpr(test::gpr_instruction(false, 32U, 32U, 28U, 29U),
		32U, 28U, 0x7fff'ffffU, 1U, positive, harness);
	test::require_gpr(test::gpr_instruction(true, 32U, 32U, 28U, 29U),
		32U, 28U, 0x8000'0000U, 0U, positive, harness);

	auto negative = base::state(41U);
	test::set_gpr_source(negative, 29U, 32U, 0xcf00'0000U);
	test::require_gpr(test::gpr_instruction(false, 32U, 32U, 28U, 29U),
		32U, 28U, 0x8000'0000U, 0U, negative, harness);
	test::require_gpr(test::gpr_instruction(true, 32U, 32U, 28U, 29U),
		32U, 28U, 0U, 1U, negative, harness);
}

} // namespace

int main()
{
	truncation_sign_and_w_zero_extension();
	saturation_and_invalid_rules();
	return 0;
}
