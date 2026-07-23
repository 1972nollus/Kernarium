// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace test = arm64_simd_test;

namespace {

struct Registers final {
	unsigned destination;
	unsigned source;
};

constexpr std::array register_cases{
	Registers{0, 1}, Registers{1, 1}, Registers{1, 0}, Registers{31, 30},
};
constexpr std::array opcodes{3U, 10U, 26U, 27U};

[[nodiscard]] constexpr std::uint32_t reduction(bool q, bool unsigned_group,
	unsigned size, unsigned opcode, Registers registers)
{
	return 0x0e30'0800U | (q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) | ((opcode & 31U) << 12) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] std::uint64_t read_lane(const test::arm64::CpuState &cpu,
	unsigned reg, std::size_t bytes)
{
	std::uint64_t value = 0;
	for (std::size_t index = 0; index < bytes; ++index)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(cpu.vectors[reg].bytes[index])) <<
			(index * 8);
	return value;
}

void every_operation_shape_and_alias()
{
	test::Harness harness;
	for (const bool q : {false, true})
		for (const bool unsigned_group : {false, true})
			for (unsigned size = 0; size < 3; ++size) {
				if (!q && size == 2U)
					continue;
				for (const auto opcode : opcodes) {
					if (opcode == 27U && unsigned_group)
						continue;
					for (const auto registers : register_cases)
						for (unsigned seed = 0; seed < 3; ++seed)
							harness.require_differential(reduction(q,
								unsigned_group, size, opcode, registers),
								test::state(seed + opcode * 3U + size));
				}
			}
}

void signed_unsigned_and_wrapping_known_answers()
{
	test::Harness harness;
	auto initial = test::state();
	initial.vectors[1].bytes.fill(std::byte{});
	constexpr std::array values{
		std::uint8_t{0x80}, std::uint8_t{0x7f}, std::uint8_t{0xff},
		std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{3},
		std::uint8_t{4}, std::uint8_t{5},
	};
	for (unsigned index = 0; index < values.size(); ++index)
		initial.vectors[1].bytes[index] = static_cast<std::byte>(values[index]);

	const Registers registers{0, 1};
	const auto saddlv = harness.run_interpreter(reduction(false, false, 0, 3,
		registers), initial);
	const auto uaddlv = harness.run_interpreter(reduction(false, true, 0, 3,
		registers), initial);
	test::require(read_lane(saddlv, 0, 2) == 0x000dU);
	test::require(read_lane(uaddlv, 0, 2) == 0x020dU);

	const auto smaxv = harness.run_interpreter(reduction(false, false, 0, 10,
		registers), initial);
	const auto umaxv = harness.run_interpreter(reduction(false, true, 0, 10,
		registers), initial);
	const auto sminv = harness.run_interpreter(reduction(false, false, 0, 26,
		registers), initial);
	const auto uminv = harness.run_interpreter(reduction(false, true, 0, 26,
		registers), initial);
	test::require(read_lane(smaxv, 0, 1) == 0x7fU);
	test::require(read_lane(umaxv, 0, 1) == 0xffU);
	test::require(read_lane(sminv, 0, 1) == 0x80U);
	test::require(read_lane(uminv, 0, 1) == 0x01U);

	initial.vectors[1].bytes.fill(std::byte{0x80});
	const auto addv = harness.run_interpreter(reduction(false, false, 0, 27,
		registers), initial);
	test::require(read_lane(addv, 0, 1) == 0);
	for (unsigned byte = 1; byte < 16; ++byte)
		test::require(addv.vectors[0].bytes[byte] == std::byte{});

	for (const auto instruction : {
		reduction(false, false, 0, 3, registers),
		reduction(false, true, 0, 3, registers),
		reduction(false, false, 0, 10, registers),
		reduction(false, true, 0, 10, registers),
		reduction(false, false, 0, 26, registers),
		reduction(false, true, 0, 26, registers),
		reduction(false, false, 0, 27, registers),
	})
		harness.require_differential(instruction, initial);
}

} // namespace

int main()
{
	every_operation_shape_and_alias();
	signed_unsigned_and_wrapping_known_answers();
	return 0;
}
