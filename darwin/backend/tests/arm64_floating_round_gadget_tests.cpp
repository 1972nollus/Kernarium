// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_round_test_support.hpp"

#include <array>

namespace test = arm64_floating_round_test;

namespace {

void every_destructive_register_alias(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 30'000U;
	test::for_each_form([&](const test::Form &form) {
		const auto format = test::format(form.width);
		for (const auto registers : test::register_cases) {
			auto initial = test::state(++seed);
			initial.fpcr = (seed & 3U) << 22;
			const std::array inputs{
				test::dyadic_bits(format, 5U, -1),
				test::dyadic_bits(format, 9U, -2, true),
				test::zero_bits(format, true),
				test::dyadic_bits(format, 7U, -1),
				test::minimum_subnormal_bits(format),
				test::quiet_nan_bits(format, true),
				test::dyadic_bits(format, 3U, -1),
				test::signaling_nan_bits(format),
			};
			const unsigned lanes = form.scalar ? 1U :
				form.vector_bytes / (form.width / 8U);
			for (unsigned lane = 0; lane < lanes; ++lane)
				test::write_lane(
					initial.vectors[registers.source].bytes,
					lane, form.width, inputs[lane]);
			harness.check_one("floating-round-register-alias",
				test::instruction(form, registers), initial,
				test::expected_state(initial, form, registers),
				test::native_requirements(form));
		}
	});
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_destructive_register_alias(harness);
	harness.print_summary("arm64-floating-round-registers");
	return 0;
}
