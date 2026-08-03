// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_shift_immediate_test_support.hpp"

#include <array>

namespace test = arm64_simd_shift_immediate_test;

namespace {

constexpr std::array forms{
	test::Form::vector_64,
	test::Form::vector_128,
	test::Form::scalar,
};

void ordinary_left_shifts(test::engine_tests::EngineHarness &harness)
{
	for (const auto operation :
	     {test::Operation::shl, test::Operation::sli})
		for (const auto form : forms)
			for (unsigned encoded = 8; encoded < 128U; ++encoded) {
				if (!test::valid(operation, form, encoded))
					continue;
				const unsigned lane_bits = std::bit_floor(encoded);
				const std::size_t active_bytes =
					form == test::Form::scalar ? lane_bits / 8U :
					form == test::Form::vector_128 ? 16U : 8U;
				for (const bool alias : {false, true})
					for (unsigned pattern = 0; pattern < 4U; ++pattern) {
						const unsigned destination = alias ? 22U : 21U;
						const unsigned source = 22U;
						auto initial = test::state(encoded +
							static_cast<unsigned>(operation) * 13U +
							pattern * 41U + alias * 73U);
						test::set_lane_pattern(initial, source,
							lane_bits, active_bytes, pattern);
						if (!alias)
							test::set_lane_pattern(initial, destination,
								lane_bits, active_bytes,
								(pattern + 1U) & 3U);
						harness.check_one(
							"ordinary shift-left-immediate behavior",
							test::instruction(form, operation, encoded,
								destination, source),
							initial,
							test::expected_state(initial, form,
								operation, encoded, destination, source),
							test::native_requirements());
					}
			}
}

void saturating_left_shifts(test::engine_tests::EngineHarness &harness)
{
	for (const auto operation : {test::Operation::sqshl,
	     test::Operation::uqshl, test::Operation::sqshlu})
		for (const auto form : forms)
			for (unsigned encoded = 8; encoded < 128U; ++encoded) {
				if (!test::valid(operation, form, encoded))
					continue;
				const unsigned lane_bits = std::bit_floor(encoded);
				const std::size_t active_bytes =
					form == test::Form::scalar ? lane_bits / 8U :
					form == test::Form::vector_128 ? 16U : 8U;
				for (const bool alias : {false, true})
					for (unsigned pattern = 0; pattern < 4U; ++pattern) {
						const unsigned destination = alias ? 24U : 23U;
						const unsigned source = 24U;
						auto initial = test::state(encoded +
							static_cast<unsigned>(operation) * 17U +
							pattern * 43U + alias * 79U);
						test::set_lane_pattern(initial, source,
							lane_bits, active_bytes, pattern);
						if (!alias)
							test::set_lane_pattern(initial, destination,
								lane_bits, active_bytes,
								(pattern + 3U) & 3U);
						if (pattern == 3U)
							initial.fpsr |= test::qc;
						harness.check_one(
							"saturating shift-left-immediate behavior",
							test::instruction(form, operation, encoded,
								destination, source),
							initial,
							test::expected_state(initial, form,
								operation, encoded, destination, source),
							test::native_requirements());
					}
			}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	ordinary_left_shifts(harness);
	saturating_left_shifts(harness);
	harness.print_summary("simd-shift-immediate-left");
	return 0;
}
