// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtxn_test_support.hpp"

namespace test = arm64_simd_fcvtxn_test;
namespace base = arm64_simd_test;

namespace {

void ignores_fpcr_rounding_mode()
{
	base::Harness harness;
	for (unsigned mode = 0; mode < 4U; ++mode) {
		auto initial = base::state(mode);
		initial.fpcr = mode << 22;
		test::set_source_lane(initial, 29U, 0U,
			0x3ff0'0000'1000'0000ULL);
		const std::uint32_t outputs[]{0x3f80'0001U};
		test::require_result(test::instruction(test::Form::scalar, 28U, 29U),
			test::Form::scalar, 28U, outputs, 1U << 4, initial, harness);
	}
}

void overflow_and_underflow_jam_to_odd()
{
	base::Harness harness;
	for (const auto form : {test::Form::scalar, test::Form::vector_lower,
	     test::Form::vector_upper}) {
		const unsigned count = test::lanes(form);
		auto overflow = base::state(20U + static_cast<unsigned>(form));
		std::array<std::uint32_t, 2> maximum{};
		for (unsigned lane = 0; lane < count; ++lane) {
			test::set_source_lane(overflow, 29U, lane,
				0x7fe0'0000'0000'0000ULL);
			maximum[lane] = 0x7f7f'ffffU;
		}
		test::require_result(test::instruction(form, 28U, 29U), form, 28U,
			std::span{maximum}.first(count), (1U << 2) | (1U << 4),
			overflow, harness);

		auto underflow = base::state(30U + static_cast<unsigned>(form));
		std::array<std::uint32_t, 2> minimum{};
		for (unsigned lane = 0; lane < count; ++lane) {
			test::set_source_lane(underflow, 29U, lane,
				0x3690'0000'0000'0000ULL);
			minimum[lane] = 1U;
		}
		test::require_result(test::instruction(form, 28U, 29U), form, 28U,
			std::span{minimum}.first(count), (1U << 3) | (1U << 4),
			underflow, harness);
	}
}

} // namespace

int main()
{
	ignores_fpcr_rounding_mode();
	overflow_and_underflow_jam_to_odd();
	return 0;
}
