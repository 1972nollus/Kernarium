// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t addp_scalar(
	unsigned destination, unsigned source)
{
	return 0x5ef1'b800U | ((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] std::uint64_t half(
	const test::arm64::VectorRegister &vector, unsigned index)
{
	std::uint64_t result;
	std::memcpy(&result, vector.bytes.data() + index * sizeof(result),
		sizeof(result));
	return result;
}

void wrapping_sum_alias_and_zeroing()
{
	test::Harness harness;
	for (const auto registers : {std::array{0U, 1U}, std::array{1U, 1U},
		std::array{31U, 30U}})
		for (unsigned seed = 0; seed < 128; ++seed) {
			const auto initial = test::state(seed);
			const auto expected = half(initial.vectors[registers[1]], 0) +
				half(initial.vectors[registers[1]], 1);
			const auto instruction = addp_scalar(registers[0], registers[1]);
			const auto result = harness.run_interpreter(instruction, initial);
			test::require(half(result.vectors[registers[0]], 0) == expected);
			test::require(half(result.vectors[registers[0]], 1) == 0U);
			harness.require_differential(instruction, initial);
		}
}

} // namespace

int main()
{
	wrapping_sum_alias_and_zeroing();
	return 0;
}
