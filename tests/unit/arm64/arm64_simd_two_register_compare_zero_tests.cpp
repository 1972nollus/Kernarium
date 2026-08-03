// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_two_register_test_support.hpp"

#include <array>

namespace test = arm64_simd_two_register_test;

namespace {

constexpr std::array compare_opcodes{8U, 9U, 10U};
constexpr test::Registers registers{0U, 1U};

void exhaustive_register_fields(test::engine_tests::EngineHarness &harness)
{
	test::for_each_allocated_form(compare_opcodes,
		[&](bool scalar, bool q, bool unsigned_group,
		    unsigned size, unsigned opcode) {
			test::check_form(harness, "simd-two-register-compare-zero",
				scalar, q, unsigned_group, size, opcode);
		});
}

void negative_zero_positive(test::engine_tests::EngineHarness &harness)
{
	test::for_each_allocated_form(compare_opcodes,
		[&](bool scalar, bool q, bool unsigned_group,
		    unsigned size, unsigned opcode) {
			const std::size_t lane_size = std::size_t{1} << size;
			const unsigned lane_bits =
				static_cast<unsigned>(lane_size * 8U);
			const std::size_t active =
				scalar ? lane_size : q ? 16U : 8U;
			for (unsigned value_kind = 0; value_kind < 3U;
			     ++value_kind) {
				auto initial = test::state(
					opcode * 43U + size * 17U +
					unsigned_group * 11U + value_kind * 5U +
					scalar * 3U + q);
				initial.vectors[registers.source].bytes.fill(
					std::byte{});
				const auto value = value_kind == 0U ?
					test::mask(lane_bits) :
					value_kind == 1U ? 0U : 1U;
				for (std::size_t offset = 0; offset < active;
				     offset += lane_size)
					test::write_lane(
						initial.vectors[registers.source].bytes,
						offset, lane_size, value);
				const char *name = value_kind == 0U ?
					"simd-compare-zero-negative" :
					value_kind == 1U ?
						"simd-compare-zero-zero" :
						"simd-compare-zero-positive";
				harness.check_one(name,
					test::instruction(scalar, q,
						unsigned_group, size, opcode,
						registers),
					initial,
					test::expected_state(initial, scalar, q,
						unsigned_group, size, opcode,
						registers),
					test::native_requirements());
			}
		});
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	exhaustive_register_fields(harness);
	negative_zero_positive(harness);
	harness.print_summary("arm64-simd-two-register-compare-zero");
	return 0;
}
