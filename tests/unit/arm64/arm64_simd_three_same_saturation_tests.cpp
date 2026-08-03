// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_three_same_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace test = arm64_simd_three_same_test;

namespace {

constexpr unsigned qc = 1U << 27;

void require_model(bool condition)
{
	if (!condition) {
		std::fputs("independent saturation model check failed\n", stderr);
		std::abort();
	}
}

void force_saturation(test::arm64::CpuState &initial,
	bool unsigned_group, unsigned size, unsigned opcode)
{
	const unsigned bits = 8U << size;
	const std::uint64_t maximum = test::mask(bits);
	const std::uint64_t signed_maximum = maximum >> 1U;
	const std::uint64_t signed_minimum =
		std::uint64_t{1} << (bits - 1U);

	switch (opcode) {
	case 1:
		test::fill_lanes(initial.vectors[1],
			unsigned_group ? maximum : signed_maximum, size);
		test::fill_lanes(initial.vectors[2], 1, size);
		break;
	case 5:
		test::fill_lanes(initial.vectors[1],
			unsigned_group ? 0 : signed_minimum, size);
		test::fill_lanes(initial.vectors[2], 1, size);
		break;
	case 9:
	case 11:
		test::fill_lanes(initial.vectors[1],
			unsigned_group ? maximum : signed_maximum, size);
		test::fill_lanes(initial.vectors[2], 1, size);
		break;
	case 22:
		test::fill_lanes(initial.vectors[1], signed_minimum, size);
		test::fill_lanes(initial.vectors[2], signed_minimum, size);
		break;
	default:
		break;
	}
}

void qc_is_cumulative_for_every_saturating_shape(
	test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0, 1, 2};
	for (const bool scalar : {false, true})
		for (const bool q : {false, true}) {
			if (scalar && q)
				continue;
			for (const bool unsigned_group : {false, true})
				for (unsigned size = 0; size < 4; ++size)
					for (const unsigned opcode :
					     std::array{1U, 5U, 9U, 11U, 22U}) {
						if (!test::valid(scalar, q, unsigned_group,
						    size, opcode))
							continue;

						auto initial = test::state(
							size + opcode * 7U + unsigned_group * 37U +
							q * 53U + scalar * 71U);
						force_saturation(
							initial, unsigned_group, size, opcode);
						initial.fpsr = 1U;
						auto expected = test::expected_state(
							initial, scalar, q, unsigned_group, size,
							opcode, registers);
						require_model((expected.fpsr & qc) != 0);
						require_model((expected.fpsr & 1U) != 0);
						harness.check_one("simd-saturation-sets-qc",
							test::instruction(scalar, q, unsigned_group,
								size, opcode, registers),
							initial, expected,
							test::native_requirements());

						test::fill_lanes(initial.vectors[1], 0, size);
						test::fill_lanes(initial.vectors[2], 0, size);
						initial.fpsr = qc | 1U;
						expected = test::expected_state(
							initial, scalar, q, unsigned_group, size,
							opcode, registers);
						require_model((expected.fpsr & (qc | 1U)) ==
							(qc | 1U));
						harness.check_one(
							"simd-saturation-preserves-prior-qc",
							test::instruction(scalar, q, unsigned_group,
								size, opcode, registers),
							initial, expected,
							test::native_requirements());

						initial.fpsr = 1U;
						expected = test::expected_state(
							initial, scalar, q, unsigned_group, size,
							opcode, registers);
						require_model((expected.fpsr & qc) == 0);
						require_model((expected.fpsr & 1U) != 0);
						harness.check_one(
							"simd-non-saturation-leaves-qc-clear",
							test::instruction(scalar, q, unsigned_group,
								size, opcode, registers),
							initial, expected,
							test::native_requirements());
					}
		}
}

void shift_extremes(test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0, 1, 2};
	for (unsigned size = 0; size < 4; ++size) {
		const bool q = size == 3U;
		const unsigned bits = 8U << size;
		auto initial = test::state(size);
		test::fill_lanes(initial.vectors[1],
			std::uint64_t{1} << (bits - 1U), size);
		test::fill_lanes(initial.vectors[2], bits - 1U, size);
		for (const unsigned opcode : {9U, 11U}) {
			const auto expected = test::expected_state(
				initial, false, q, false, size, opcode, registers);
			require_model((expected.fpsr & qc) != 0);
			harness.check_one("simd-signed-left-shift-extreme",
				test::instruction(false, q, false, size, opcode,
					registers),
				initial, expected, test::native_requirements());
		}
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	qc_is_cumulative_for_every_saturating_shape(harness);
	shift_extremes(harness);
	harness.print_summary("arm64-simd-three-same-saturation");
	return 0;
}
