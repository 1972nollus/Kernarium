// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvt_precision_test_support.hpp"

namespace test = arm64_simd_fcvt_precision_test;
namespace base = arm64_simd_test;

namespace {

void nan_payload_default_nan_and_signaling()
{
	base::Harness harness;
	for (const bool signaling : {false, true}) {
		auto initial = base::state(signaling);
		initial.fpsr = 0x0800'0090U;
		test::set_source(initial, 29U, 16U, signaling ? 0x7d51U : 0x7e55U);
		test::require_result(test::instruction(16U, 32U, 28U, 29U),
			32U, 28U, signaling ? 0x7fea'2000U : 0x7fca'a000U,
			signaling ? 1U : 0U, initial, harness);
	}

	for (const unsigned destination_width : {16U, 64U}) {
		auto dn = base::state(destination_width);
		dn.fpcr = 1U << 25;
		test::set_source(dn, 29U, 32U, test::quiet_nan(32U, true));
		test::require_result(test::instruction(32U, destination_width,
			28U, 29U), destination_width, 28U,
			destination_width == 16U ? 0x7e00U :
				0x7ff8'0000'0000'0000ULL,
			0U, dn, harness);

		auto dn_ah = dn;
		dn_ah.fpcr |= 2U;
		test::require_result(test::instruction(32U, destination_width,
			28U, 29U), destination_width, 28U,
			(destination_width == 16U ? 0xfe00U :
				0xfff8'0000'0000'0000ULL),
			0U, dn_ah, harness);
	}
}

void source_denormal_fz_fiz_and_ah()
{
	base::Harness harness;
	struct Entry final {
		std::uint32_t fpcr;
		std::uint64_t output;
		std::uint32_t exceptions;
	};
	const Entry entries[]{
		{0U, 0x36a0'0000'0000'0000ULL, 0U},
		{1U << 24, 0U, 1U << 7},
		{1U, 0U, 0U},
		{(1U << 24) | 1U, 0U, 1U << 7},
		{2U, 0x36a0'0000'0000'0000ULL, 1U << 7},
		{(1U << 24) | 2U, 0x36a0'0000'0000'0000ULL, 1U << 7},
		{3U, 0U, 0U},
	};
	for (const auto entry : entries) {
		auto initial = base::state(entry.fpcr);
		initial.fpcr = entry.fpcr;
		initial.fpsr = 0x10U;
		test::set_source(initial, 29U, 32U, 1U);
		test::require_result(test::instruction(32U, 64U, 28U, 29U),
			64U, 28U, entry.output, entry.exceptions, initial, harness);
	}
}

void output_flush_timing_under_ah()
{
	base::Harness harness;
	for (const bool ah : {false, true}) {
		auto initial = base::state(ah);
		initial.fpcr = (1U << 24) | (ah ? 2U : 0U);
		initial.fpsr = 0x80U;
		test::set_source(initial, 29U, 64U, 0x36a0'0000'0000'0000ULL);
		test::require_result(test::instruction(64U, 32U, 28U, 29U),
			32U, 28U, 0U, ah ? (1U << 3) | (1U << 4) : 1U << 3,
			initial, harness);
	}
}

void alternative_half_source_and_destination()
{
	base::Harness harness;
	auto source = base::state(1U);
	source.fpcr = 1U << 26;
	test::set_source(source, 29U, 16U, 0x7c00U);
	test::require_result(test::instruction(16U, 32U, 28U, 29U),
		32U, 28U, 0x4780'0000U, 0U, source, harness);

	auto finite = base::state(2U);
	finite.fpcr = 1U << 26;
	test::set_source(finite, 29U, 32U, 0x4780'0000U); /* 65536 */
	test::require_result(test::instruction(32U, 16U, 28U, 29U),
		16U, 28U, 0x7c00U, 0U, finite, harness);

	auto finite_overflow = base::state(22U);
	finite_overflow.fpcr = 1U << 26;
	test::set_source(finite_overflow, 29U, 32U, 0x4843'5000U); /* 200000 */
	test::require_result(test::instruction(32U, 16U, 28U, 29U),
		16U, 28U, 0x7fffU, 1U, finite_overflow, harness);

	for (const bool negative : {false, true}) {
		auto special = base::state(3U + negative);
		special.fpcr = 1U << 26;
		test::set_source(special, 29U, 32U,
			test::infinity(32U, negative));
		test::require_result(test::instruction(32U, 16U, 28U, 29U),
			16U, 28U, negative ? 0xffffU : 0x7fffU, 1U,
			special, harness);

		auto nan = special;
		test::set_source(nan, 29U, 32U, test::quiet_nan(32U, negative));
		test::require_result(test::instruction(32U, 16U, 28U, 29U),
			16U, 28U, negative ? 0x8000U : 0U, 1U, nan, harness);
	}
}

} // namespace

int main()
{
	nan_payload_default_nan_and_signaling();
	source_denormal_fz_fiz_and_ah();
	output_flush_timing_under_ah();
	alternative_half_source_and_destination();
	return 0;
}
