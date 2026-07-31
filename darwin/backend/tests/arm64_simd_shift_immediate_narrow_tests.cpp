// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_shift_immediate_test_support.hpp"

#include <array>

namespace test = arm64_simd_shift_immediate_test;

namespace {

constexpr std::array vector_forms{
	test::Form::vector_64,
	test::Form::vector_128,
};

constexpr std::array saturating_operations{
	test::Operation::sqshrn,
	test::Operation::sqrshrn,
	test::Operation::sqshrun,
	test::Operation::sqrshrun,
	test::Operation::uqshrn,
	test::Operation::uqrshrn,
};

void ordinary_narrowing(test::engine_tests::EngineHarness &harness)
{
	for (const auto operation :
	     {test::Operation::shrn, test::Operation::rshrn})
		for (const auto form : vector_forms)
			for (unsigned encoded = 8; encoded < 64U; ++encoded)
				for (const bool alias : {false, true})
					for (unsigned pattern = 0; pattern < 4U; ++pattern) {
						const unsigned destination = alias ? 26U : 25U;
						const unsigned source = 26U;
						const unsigned source_bits =
							std::bit_floor(encoded) * 2U;
						auto initial = test::state(encoded +
							static_cast<unsigned>(operation) * 19U +
							pattern * 47U + alias * 83U);
						test::set_lane_pattern(initial, source,
							source_bits, 16U, pattern);
						if (!alias)
							test::set_lane_pattern(initial, destination,
								source_bits / 2U, 16U,
								(pattern + 2U) & 3U);
						harness.check_one(
							"ordinary narrowing shift behavior",
							test::instruction(form, operation, encoded,
								destination, source),
							initial,
							test::expected_state(initial, form,
								operation, encoded, destination, source),
							test::native_requirements());
					}
}

void saturating_narrowing(test::engine_tests::EngineHarness &harness)
{
	constexpr std::array forms{
		test::Form::vector_64,
		test::Form::vector_128,
		test::Form::scalar,
	};
	for (const auto operation : saturating_operations)
		for (const auto form : forms)
			for (unsigned encoded = 8; encoded < 64U; ++encoded)
				for (const bool alias : {false, true})
					for (unsigned pattern = 0; pattern < 4U; ++pattern) {
						const unsigned destination = alias ? 28U : 27U;
						const unsigned source = 28U;
						const unsigned source_bits =
							std::bit_floor(encoded) * 2U;
						const std::size_t source_bytes =
							form == test::Form::scalar ?
								source_bits / 8U : 16U;
						auto initial = test::state(encoded +
							static_cast<unsigned>(operation) * 23U +
							pattern * 53U + alias * 89U);
						test::set_lane_pattern(initial, source,
							source_bits, source_bytes, pattern);
						if (!alias)
							test::set_lane_pattern(initial, destination,
								source_bits / 2U, 16U,
								(pattern + 1U) & 3U);
						if (pattern == 3U)
							initial.fpsr |= test::qc;
						harness.check_one(
							"saturating narrowing shift behavior",
							test::instruction(form, operation, encoded,
								destination, source),
							initial,
							test::expected_state(initial, form,
								operation, encoded, destination, source),
							test::native_requirements());
					}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	ordinary_narrowing(harness);
	saturating_narrowing(harness);
	harness.print_summary("simd-shift-immediate-narrow");
	return 0;
}
