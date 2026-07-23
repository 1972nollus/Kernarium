// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <array>
#include <cstddef>

namespace test = arm64_simd_test;

namespace {

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

constexpr std::array register_cases{
	Registers{0, 1, 2}, Registers{1, 1, 2}, Registers{2, 1, 2},
	Registers{1, 1, 1}, Registers{31, 30, 29},
};

[[nodiscard]] constexpr std::uint32_t extract(bool q, unsigned offset,
	Registers registers)
{
	return 0x2e00'0000U | (q ? 0x4000'0000U : 0U) |
		((registers.right & 31U) << 16) | ((offset & 15U) << 11) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

void every_offset_width_and_alias()
{
	test::Harness harness;
	for (const bool q : {false, true}) {
		const unsigned bytes = q ? 16U : 8U;
		for (unsigned offset = 0; offset < bytes; ++offset)
			for (const auto registers : register_cases)
				for (unsigned seed = 0; seed < 4; ++seed) {
					auto initial = test::state(seed + offset * 11U);
					const auto left = initial.vectors[registers.left].bytes;
					const auto right = initial.vectors[registers.right].bytes;
					const auto instruction = extract(q, offset, registers);
					const auto result = harness.run_interpreter(
						instruction, initial);
					for (unsigned output = 0; output < bytes; ++output) {
						const unsigned source = offset + output;
						const auto expected = source < bytes ? left[source] :
							right[source - bytes];
						test::require(result.vectors[registers.destination]
							.bytes[output] == expected);
					}
					for (unsigned output = bytes; output < 16; ++output)
						test::require(result.vectors[registers.destination]
							.bytes[output] == std::byte{});
					harness.require_differential(instruction, initial);
				}
	}
}

} // namespace

int main()
{
	every_offset_width_and_alias();
	return 0;
}
