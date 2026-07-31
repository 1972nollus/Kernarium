// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_reciprocal_test_support.hpp"

namespace test = arm64_floating_reciprocal_test;

namespace {

void every_reserved_one_double_shape(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 0;
	for (const auto operation : {
		     test::Operation::reciprocal_estimate,
		     test::Operation::reciprocal_step,
		     test::Operation::reciprocal_sqrt_estimate,
		     test::Operation::reciprocal_sqrt_step,
	     }) {
		const bool unary = test::unary(operation);
		const test::Form allocated{operation, false, 64U, 16U};
		for (unsigned first = 0; first < 32U; ++first)
			for (unsigned second = 0; second < (unary ? 1U : 32U);
			     ++second)
				for (unsigned destination = 0; destination < 32U;
				     ++destination) {
					const test::Registers registers{
						destination, first, second};
					const auto reserved =
						test::instruction(allocated, registers) &
						~0x4000'0000U;
					harness.check_illegal(
						"floating-reciprocal-reserved-1d",
						reserved, test::state(++seed));
				}
	}
}

[[nodiscard]] test::ExpectedEngineCounts expected_counts()
{
	test::ExpectedEngineCounts expected;
	constexpr std::size_t unary_operations = 2U;
	constexpr std::size_t binary_operations = 2U;
	constexpr std::size_t unary_register_fields = 32U * 32U;
	constexpr std::size_t binary_register_fields = 32U * 32U * 32U;
	constexpr std::size_t cases =
		unary_operations * unary_register_fields +
		binary_operations * binary_register_fields;
	expected.add(test::engine_tests::NativeRequirements::baseline(), cases);
	test::require(expected.total == 67'584U,
		"reserved .1D formula omitted an encoding");
	return expected;
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_reserved_one_double_shape(harness);
	test::require_engine_counts(harness, expected_counts());
	harness.print_summary("arm64-floating-reciprocal-reserved");
	return 0;
}
