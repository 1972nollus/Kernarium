// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_scale_test_support.hpp"

namespace test = arm64_floating_scale_test;

namespace {

void every_valid_register_encoding(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	for (const auto &form : test::forms)
		for (unsigned destination = 0; destination < 32U; ++destination)
			for (unsigned first = 0; first < 32U; ++first)
				for (unsigned second = 0; second < 32U; ++second) {
					const test::Registers registers{
						destination, first, second};
					const auto initial = test::state(++seed);
					harness.check_one("floating-scale-valid-allocation",
						test::instruction(form, registers), initial,
						test::expected_state(initial, form, registers),
						test::native_requirements());
				}
}

void every_reserved_double_register_encoding(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 200'000U;
	for (unsigned destination = 0; destination < 32U; ++destination)
		for (unsigned first = 0; first < 32U; ++first)
			for (unsigned second = 0; second < 32U; ++second) {
				const test::Registers registers{
					destination, first, second};
				/*
				 * Q=0,size=1 is the one reserved arrangement in the
				 * single/double encoding class.
				 */
				auto encoded = test::instruction(
					{64U, 16U}, registers);
				encoded &= ~0x4000'0000U;
				harness.check_illegal(
					"floating-scale-reserved-double-arrangement",
					encoded, test::state(++seed));
			}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_valid_register_encoding(harness);
	every_reserved_double_register_encoding(harness);
	harness.print_summary("arm64-floating-scale-allocation");
	return 0;
}
