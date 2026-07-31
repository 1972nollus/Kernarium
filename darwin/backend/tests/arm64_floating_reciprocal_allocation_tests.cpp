// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_reciprocal_test_support.hpp"

namespace test = arm64_floating_reciprocal_test;

namespace {

void every_valid_register_field(test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 0;
	test::for_each_form([&](const test::Form &form) {
		for (unsigned first = 0; first < 32U; ++first)
			for (unsigned second = 0;
			     second < (test::unary(form.operation) ? 1U : 32U);
			     ++second)
				for (unsigned destination = 0; destination < 32U;
				     ++destination) {
					const test::Registers registers{
						destination, first, second};
					auto initial = test::state(++seed);
					test::prepare_simple(initial, form, registers);
					test::check_one(harness, form, registers, initial);
				}
	});
}

[[nodiscard]] test::ExpectedEngineCounts expected_counts()
{
	test::ExpectedEngineCounts expected;
	test::for_each_form([&](const test::Form &form) {
		const std::size_t register_allocations =
			32U * 32U *
			(test::unary(form.operation) ? 1U : 32U);
		expected.add(form, 0U, register_allocations);
	});
	test::require(expected.total == 543'744U,
		"reciprocal allocation formula omitted a valid field");
	return expected;
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_valid_register_field(harness);
	test::require_engine_counts(harness, expected_counts());
	harness.print_summary("arm64-floating-reciprocal-allocation");
	return 0;
}
