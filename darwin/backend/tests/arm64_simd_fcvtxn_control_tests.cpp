// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtxn_test_support.hpp"

namespace test = arm64_simd_fcvtxn_test;
namespace base = arm64_simd_test;

namespace {

void nan_payload_default_nan_and_signaling()
{
	base::Harness harness;
	for (const bool signaling : {false, true}) {
		auto initial = base::state(signaling);
		initial.fpsr = 0x0800'0090U;
		test::set_source_lane(initial, 29U, 0U,
			signaling ? 0x7ff0'1234'5678'9abcULL :
				0x7ff8'1234'5678'9abcULL);
		const std::uint32_t outputs[]{0x7fc0'91a2U};
		test::require_result(test::instruction(test::Form::scalar, 28U, 29U),
			test::Form::scalar, 28U, outputs, signaling ? 1U : 0U,
			initial, harness);
	}

	for (const bool ah : {false, true}) {
		auto initial = base::state(10U + ah);
		initial.fpcr = (1U << 25) | (ah ? 2U : 0U);
		test::set_source_lane(initial, 29U, 0U,
			0xfff8'1234'5678'9abcULL);
		const std::uint32_t outputs[]{ah ? 0xffc0'0000U : 0x7fc0'0000U};
		test::require_result(test::instruction(test::Form::scalar, 28U, 29U),
			test::Form::scalar, 28U, outputs, 0U, initial, harness);
	}
}

void afp_subnormal_controls()
{
	base::Harness harness;
	struct Entry final {
		std::uint32_t fpcr;
		std::uint32_t output;
		std::uint32_t exceptions;
	};
	const Entry entries[]{
		{0U, 1U, (1U << 3) | (1U << 4)},
		{1U << 24, 0U, 1U << 7},
		{1U, 0U, 0U},
		{2U, 1U, (1U << 7) | (1U << 3) | (1U << 4)},
		{(1U << 24) | 2U, 0U,
			(1U << 7) | (1U << 3) | (1U << 4)},
		{3U, 0U, 0U},
	};
	for (const auto entry : entries) {
		auto initial = base::state(entry.fpcr);
		initial.fpcr = entry.fpcr;
		initial.fpsr = 0x10U;
		test::set_source_lane(initial, 29U, 0U, 1U);
		const std::uint32_t outputs[]{entry.output};
		test::require_result(test::instruction(test::Form::scalar, 28U, 29U),
			test::Form::scalar, 28U, outputs, entry.exceptions,
			initial, harness);
	}
}

} // namespace

int main()
{
	nan_payload_default_nan_and_signaling();
	afp_subnormal_controls();
	return 0;
}
