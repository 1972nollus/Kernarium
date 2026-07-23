// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <algorithm>
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
constexpr std::array permute_opcodes{1U, 2U, 3U, 5U, 6U, 7U};

[[nodiscard]] constexpr std::uint32_t permute(bool q, unsigned size,
	unsigned opcode, Registers registers)
{
	return 0x0e00'0800U | (q ? 0x4000'0000U : 0U) |
		((size & 3U) << 22) | ((registers.right & 31U) << 16) |
		((opcode & 7U) << 12) | ((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] test::arm64::VectorRegister expected_result(
	const test::arm64::CpuState &initial, bool q, unsigned size,
	unsigned opcode, Registers registers)
{
	const std::size_t vector_size = q ? 16U : 8U;
	const std::size_t element_size = std::size_t{1} << size;
	const std::size_t elements = vector_size / element_size;
	const auto left = initial.vectors[registers.left].bytes;
	const auto right = initial.vectors[registers.right].bytes;
	test::arm64::VectorRegister result{};
	const auto copy = [&](std::size_t destination, const auto &source,
		std::size_t source_index) {
		std::copy_n(source.begin() + source_index * element_size,
			element_size,
			result.bytes.begin() + destination * element_size);
	};
	for (std::size_t index = 0; index < elements / 2; ++index) {
		if (opcode == 1U || opcode == 5U) {
			const std::size_t part = opcode == 5U ? 1U : 0U;
			copy(index, left, index * 2U + part);
			copy(index + elements / 2U, right, index * 2U + part);
		} else if (opcode == 2U || opcode == 6U) {
			const std::size_t part = opcode == 6U ? 1U : 0U;
			copy(index * 2U, left, index * 2U + part);
			copy(index * 2U + 1U, right, index * 2U + part);
		} else {
			const std::size_t base = opcode == 7U ? elements / 2U : 0U;
			copy(index * 2U, left, base + index);
			copy(index * 2U + 1U, right, base + index);
		}
	}
	return result;
}

void every_operation_shape_and_alias()
{
	test::Harness harness;
	for (const bool q : {false, true})
		for (unsigned size = 0; size < (q ? 4U : 3U); ++size)
			for (const auto opcode : permute_opcodes)
				for (const auto registers : register_cases)
					for (unsigned seed = 0; seed < 3; ++seed) {
						auto initial = test::state(
							seed + opcode * 5U + size * 17U);
						const auto instruction = permute(
							q, size, opcode, registers);
						const auto expected = expected_result(
							initial, q, size, opcode, registers);
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
	every_operation_shape_and_alias();
	return 0;
}
