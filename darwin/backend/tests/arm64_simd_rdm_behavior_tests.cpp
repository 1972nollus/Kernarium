// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_indexed_test_support.hpp"

#include <array>

namespace test = arm64_simd_indexed_test;
namespace base = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t instruction(bool scalar, bool q,
	bool subtract, unsigned size, unsigned destination,
	unsigned left, unsigned right)
{
	return (scalar ? 0x7e00'8400U : 0x2e00'8400U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(subtract ? 0x800U : 0U) | ((size & 3U) << 22) |
		((right & 31U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
}

void require_oracle(bool scalar, bool q, bool subtract, unsigned size,
	unsigned destination, unsigned left, unsigned right,
	const uml::backend::arm64::CpuState &initial, base::Harness &harness)
{
	const unsigned lane_bytes = 1U << size;
	const unsigned lane_bits = lane_bytes * 8U;
	const unsigned vector_bytes = scalar ? lane_bytes : q ? 16U : 8U;
	const auto old_destination = initial.vectors[destination].bytes;
	const auto old_left = initial.vectors[left].bytes;
	const auto old_right = initial.vectors[right].bytes;
	std::array<std::byte, 16> expected{};
	bool saturated = false;
	for (unsigned offset = 0; offset < vector_bytes; offset += lane_bytes) {
		const __int128 product = test::signed_value(
			test::read_lane(old_left, offset, lane_bytes), lane_bits) *
			test::signed_value(
				test::read_lane(old_right, offset, lane_bytes), lane_bits) * 2;
		const __int128 accumulator = test::signed_value(
			test::read_lane(old_destination, offset, lane_bytes), lane_bits);
		__int128 combined = accumulator *
			(static_cast<__int128>(1) << lane_bits) +
			(subtract ? -product : product);
		combined += static_cast<__int128>(1) << (lane_bits - 1U);
		const auto value = test::saturate_signed(
			combined >> lane_bits, lane_bits, saturated);
		test::write_lane(expected, offset, lane_bytes, value);
	}
	auto expected_state = initial;
	expected_state.pc = base::pc + 4U;
	expected_state.vectors[destination].bytes = expected;
	if (saturated)
		expected_state.fpsr |= 1U << 27;
	const auto encoded = instruction(
		scalar, q, subtract, size, destination, left, right);
	const auto interpreted = harness.run_interpreter(encoded, initial);
	base::require_architecture_equal(expected_state, interpreted);
	harness.require_differential(encoded, initial);
}

void every_shape_rounding_saturation_and_alias()
{
	base::Harness harness;
	for (const bool scalar : {false, true})
		for (const bool q : {false, true}) {
			if (scalar && q)
				continue;
			for (const bool subtract : {false, true})
				for (const unsigned size : {1U, 2U})
					for (const bool extreme : {false, true}) {
						auto initial = base::state(
							size * 17U + scalar + q + subtract + extreme);
						initial.fpsr = 0x84U;
						if (extreme) {
							const unsigned lane_bytes = 1U << size;
							const unsigned lane_bits = lane_bytes * 8U;
							const std::uint64_t minimum =
								std::uint64_t{1} << (lane_bits - 1U);
							test::set_all_lanes(initial.vectors[29].bytes,
								lane_bytes, minimum);
							test::set_all_lanes(initial.vectors[30].bytes,
								lane_bytes, minimum);
							test::set_all_lanes(initial.vectors[28].bytes,
								lane_bytes, subtract ? minimum : minimum - 1U);
						}
						require_oracle(scalar, q, subtract, size,
							28U, 29U, 30U, initial, harness);
					}
		}
	struct Registers final { unsigned destination; unsigned left; unsigned right; };
	constexpr std::array aliases{
		Registers{1U, 1U, 2U}, Registers{2U, 1U, 2U},
		Registers{1U, 2U, 2U}, Registers{3U, 3U, 3U}};
	for (const bool subtract : {false, true})
		for (const auto regs : aliases) {
			auto initial = base::state(regs.destination * 11U + subtract);
			initial.fpsr = 0x0800'0084U;
			require_oracle(false, true, subtract, 1U,
				regs.destination, regs.left, regs.right, initial, harness);
		}
}

} // namespace

int main()
{
	every_shape_rounding_saturation_and_alias();
	return 0;
}
