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

constexpr std::array operations{
	test::Operation::sshr,
	test::Operation::ushr,
	test::Operation::ssra,
	test::Operation::usra,
	test::Operation::srshr,
	test::Operation::urshr,
	test::Operation::srsra,
	test::Operation::ursra,
	test::Operation::sri,
};

void every_operation_shape_shift_alias_and_pattern()
{
	test::engine_tests::EngineHarness harness;
	for (const auto operation : operations)
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
						const unsigned destination = alias ? 20U : 19U;
						const unsigned source = 20U;
						auto initial = test::state(encoded +
							static_cast<unsigned>(operation) * 11U +
							pattern * 37U + alias * 71U);
						test::set_lane_pattern(initial, source,
							lane_bits, active_bytes, pattern);
						if (!alias)
							test::set_lane_pattern(initial, destination,
								lane_bits, active_bytes,
								(pattern + 2U) & 3U);
						const auto instruction = test::instruction(form,
							operation, encoded, destination, source);
						harness.check_one(
							"shift-immediate right behavior",
							instruction, initial,
							test::expected_state(initial, form,
								operation, encoded, destination, source),
							test::native_requirements());
					}
			}
	harness.print_summary("simd-shift-immediate-right");
}

} // namespace

int main()
{
	every_operation_shape_shift_alias_and_pattern();
	return 0;
}
