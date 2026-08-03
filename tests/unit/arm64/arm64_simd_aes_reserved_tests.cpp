// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_aes_test_support.hpp"

namespace test = arm64_simd_aes_test;

namespace {

void every_reserved_operation_and_register_field()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 0;
	for (unsigned operation = 0; operation < 4U; ++operation)
		for (unsigned destination = 0; destination < 32U; ++destination)
			for (unsigned source = 0; source < 32U; ++source)
				harness.check_illegal(
					"reserved block-transform operation",
					test::reserved_instruction(operation,
						{destination, source}),
					test::state(++seed));
	constexpr std::size_t expected_cases = 4U * 32U * 32U;
	static_assert(expected_cases == 4'096U);
	test::require_engine_counts(harness, 0U, expected_cases);
	harness.print_summary("arm64-simd-block-transform-reserved");
}

} // namespace

int main()
{
	every_reserved_operation_and_register_field();
	return 0;
}
