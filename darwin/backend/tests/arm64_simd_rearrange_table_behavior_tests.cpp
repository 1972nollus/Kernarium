// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <array>
#include <cstddef>

namespace test = arm64_simd_test;

namespace {

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned indices;
};

constexpr std::array register_cases{
	Registers{0, 1, 6}, Registers{1, 1, 6}, Registers{6, 1, 6},
	Registers{31, 30, 29},
};

[[nodiscard]] constexpr std::uint32_t table_lookup(bool q, bool extend,
	unsigned count, Registers registers)
{
	return 0x0e00'0000U | (q ? 0x4000'0000U : 0U) |
		((registers.indices & 31U) << 16) |
		(((count - 1U) & 3U) << 13) | (extend ? 0x1000U : 0U) |
		((registers.first & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] test::arm64::VectorRegister expected_result(
	const test::arm64::CpuState &initial, bool q, bool extend,
	unsigned count, Registers registers)
{
	const auto indices = initial.vectors[registers.indices].bytes;
	test::arm64::VectorRegister result = extend ?
		initial.vectors[registers.destination] :
		test::arm64::VectorRegister{};
	const unsigned bytes = q ? 16U : 8U;
	for (unsigned output = 0; output < bytes; ++output) {
		const unsigned index = std::to_integer<unsigned>(indices[output]);
		if (index < count * 16U) {
			const unsigned table =
				(registers.first + index / 16U) & 31U;
			result.bytes[output] = initial.vectors[table].bytes[index & 15U];
		}
	}
	for (unsigned output = bytes; output < 16; ++output)
		result.bytes[output] = std::byte{};
	return result;
}

void every_index_count_width_and_alias()
{
	test::Harness harness;
	for (const bool q : {false, true})
		for (const bool extend : {false, true})
			for (unsigned count = 1; count <= 4; ++count)
				for (const auto registers : register_cases)
					for (unsigned first_index = 0; first_index < 256;
					     ++first_index) {
						auto initial = test::state(
							first_index + count * 23U + extend * 7U);
						for (unsigned lane = 0; lane < 16; ++lane)
							initial.vectors[registers.indices].bytes[lane] =
								static_cast<std::byte>(first_index + lane * 17U);
						const auto expected = expected_result(
							initial, q, extend, count, registers);
						const auto instruction = table_lookup(
							q, extend, count, registers);
						const auto result = harness.run_interpreter(
							instruction, initial);
						test::require(result.vectors[registers.destination].bytes ==
							expected.bytes);
						harness.require_differential(instruction, initial);
					}
}

} // namespace

int main()
{
	every_index_count_width_and_alias();
	return 0;
}
