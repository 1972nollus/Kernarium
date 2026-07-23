// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvt_precision_test_support.hpp"

#include <array>

namespace test = arm64_simd_fcvt_precision_test;
namespace base = arm64_simd_test;

namespace {

struct RoundingCase final {
	unsigned source_width;
	unsigned destination_width;
	std::uint64_t input;
	std::array<std::uint64_t, 4> outputs;
};

void all_fpcr_rounding_modes_and_ties()
{
	base::Harness harness;
	const std::array cases{
		RoundingCase{32U, 16U, 0x3f80'1000U,
			{0x3c00U, 0x3c01U, 0x3c00U, 0x3c00U}},
		RoundingCase{32U, 16U, 0xbf80'1000U,
			{0xbc00U, 0xbc00U, 0xbc01U, 0xbc00U}},
		RoundingCase{64U, 32U, 0x3ff0'0000'1000'0000ULL,
			{0x3f80'0000U, 0x3f80'0001U, 0x3f80'0000U,
				0x3f80'0000U}},
		RoundingCase{64U, 32U, 0xbff0'0000'1000'0000ULL,
			{0xbf80'0000U, 0xbf80'0000U, 0xbf80'0001U,
				0xbf80'0000U}},
		RoundingCase{64U, 32U, 0x3ff0'0000'3000'0000ULL,
			{0x3f80'0002U, 0x3f80'0002U, 0x3f80'0001U,
				0x3f80'0001U}},
	};
	for (unsigned index = 0; index < cases.size(); ++index)
		for (unsigned mode = 0; mode < 4U; ++mode) {
			const auto &entry = cases[index];
			auto initial = base::state(index * 4U + mode);
			initial.fpcr = mode << 22;
			initial.fpsr = 0x0800'0080U;
			test::set_source(initial, 29U, entry.source_width, entry.input);
			test::require_result(test::instruction(entry.source_width,
				entry.destination_width, 28U, 29U), entry.destination_width,
				28U, entry.outputs[mode], 1U << 4, initial, harness);
		}
}

void overflow_and_underflow_flags()
{
	base::Harness harness;
	for (unsigned mode = 0; mode < 4U; ++mode) {
		auto overflow = base::state(mode);
		overflow.fpcr = mode << 22;
		test::set_source(overflow, 29U, 64U, 0x7fe0'0000'0000'0000ULL);
		const bool to_infinity = mode == 0U || mode == 1U;
		test::require_result(test::instruction(64U, 32U, 28U, 29U),
			32U, 28U, to_infinity ? 0x7f80'0000U : 0x7f7f'ffffU,
			(1U << 2) | (1U << 4), overflow, harness);

		auto underflow = base::state(mode + 8U);
		underflow.fpcr = mode << 22;
		test::set_source(underflow, 29U, 64U, 0x3690'0000'0000'0000ULL);
		const auto tiny = mode == 1U ? 1U : 0U;
		test::require_result(test::instruction(64U, 32U, 28U, 29U),
			32U, 28U, tiny, (1U << 3) | (1U << 4), underflow,
			harness);
	}

	auto exact_tiny = base::state(20U);
	test::set_source(exact_tiny, 29U, 64U, 0x36a0'0000'0000'0000ULL);
	test::require_result(test::instruction(64U, 32U, 28U, 29U),
		32U, 28U, 1U, 0U, exact_tiny, harness);

	/* This exact midpoint rounds from the subnormal range to the minimum
	 * normal single. Standard mode detects underflow before rounding; AH
	 * deliberately changes the detection point to after rounding. */
	for (const bool ah : {false, true}) {
		auto boundary = base::state(30U + ah);
		boundary.fpcr = ah ? 2U : 0U;
		test::set_source(boundary, 29U, 64U,
			0x380f'ffff'f000'0000ULL);
		test::require_result(test::instruction(64U, 32U, 28U, 29U),
			32U, 28U, 0x0080'0000U,
			(ah ? 0U : 1U << 3) | (1U << 4), boundary, harness);
	}
}

} // namespace

int main()
{
	all_fpcr_rounding_modes_and_ties();
	overflow_and_underflow_flags();
	return 0;
}
