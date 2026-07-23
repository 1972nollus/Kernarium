// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fjcvt_test_support.hpp"

namespace test = arm64_simd_fjcvt_test;
namespace base = arm64_simd_test;

namespace {

struct Control final {
	std::uint32_t fpcr;
	std::uint32_t nzcv;
	std::uint32_t exceptions;
};

void nan_infinity_and_subnormal_controls()
{
	base::Harness harness;
	for (const auto input : {0x7ff8'1234'5678'9abcULL,
			0x7ff0'0000'0000'0000ULL, 0xfff0'0000'0000'0000ULL}) {
		auto initial = base::state(static_cast<unsigned>(input));
		test::set_source(initial, input);
		test::require_result(28U, 0U, 0U, 1U, initial, harness);
	}

	for (const auto control : {
			Control{0U, 0U, 1U << 4},
			Control{1U << 24, 0x4000'0000U, 1U << 7},
			Control{1U, 0x4000'0000U, 0U},
			Control{2U, 0U, (1U << 7) | (1U << 4)}}) {
		auto initial = base::state(control.fpcr);
		initial.fpcr = control.fpcr;
		test::set_source(initial, 1U);
		test::require_result(28U, 0U, control.nzcv, control.exceptions,
			initial, harness);
	}
}

} // namespace

int main()
{
	nan_infinity_and_subnormal_controls();
	return 0;
}
