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

constexpr std::array integer_opcodes{2U, 3U, 4U, 6U, 7U, 11U, 18U, 19U,
	20U};

[[nodiscard]] constexpr std::uint32_t two_register(bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode, Registers registers)
{
	return (scalar ? 0x5e20'0800U : 0x0e20'0800U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) | ((opcode & 31U) << 12) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr bool valid_integer(bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode)
{
	switch (opcode) {
	case 2:
	case 4:
	case 6:
		return !scalar && size < 3U;
	case 3:
	case 7:
		return scalar || q || size < 3U;
	case 11:
		return scalar ? size == 3U : q || size < 3U;
	case 18:
		return size < 3U && (unsigned_group || !scalar);
	case 19:
		return size < 3U && unsigned_group && !scalar;
	case 20:
		return size < 3U;
	default:
		return false;
	}
}

void write_lane(test::arm64::CpuState &cpu, unsigned reg, std::size_t offset,
	std::size_t bytes, std::uint64_t value)
{
	for (std::size_t index = 0; index < bytes; ++index)
		cpu.vectors[reg].bytes[offset + index] = static_cast<std::byte>(
			value >> (index * 8));
}

[[nodiscard]] std::uint64_t read_lane(const test::arm64::CpuState &cpu,
	unsigned reg, std::size_t offset, std::size_t bytes)
{
	std::uint64_t value = 0;
	for (std::size_t index = 0; index < bytes; ++index)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(cpu.vectors[reg].bytes[offset + index])) <<
			(index * 8);
	return value;
}

void every_shape_operation_and_alias()
{
	test::Harness harness;
	for (const bool scalar : {false, true})
		for (const bool q : {false, true}) {
			if (scalar && !q)
				continue;
			for (const bool unsigned_group : {false, true})
				for (unsigned size = 0; size < 4; ++size)
					for (const auto opcode : integer_opcodes) {
						if (!valid_integer(scalar, q, unsigned_group,
						    size, opcode))
							continue;
						for (const auto registers : register_cases)
							for (unsigned seed = 0; seed < 2; ++seed)
								harness.require_differential(two_register(
									scalar, q, unsigned_group, size, opcode,
									registers), test::state(seed + opcode * 3U + size));
					}
		}

	for (const bool q : {false, true})
		for (const auto registers : register_cases) {
			for (unsigned size = 0; size < 3; ++size)
				harness.require_differential(two_register(false, q, false,
					size, 0, registers), test::state(size)); // REV64
			for (unsigned size = 0; size < 2; ++size)
				harness.require_differential(two_register(false, q, true,
					size, 0, registers), test::state(size + 3U)); // REV32
			harness.require_differential(two_register(false, q, false, 0, 1,
				registers), test::state(7)); // REV16
			harness.require_differential(two_register(false, q, false, 0, 5,
				registers), test::state(8)); // CNT
			harness.require_differential(two_register(false, q, true, 0, 5,
				registers), test::state(9)); // NOT
			harness.require_differential(two_register(false, q, true, 1, 5,
				registers), test::state(10)); // RBIT
		}

	for (const bool scalar : {false, true})
		for (const bool q : {false, true}) {
			if (scalar && !q)
				continue;
			for (const bool alternate : {false, true})
				for (unsigned size = 0; size < 4; ++size) {
					const bool shape = scalar ? size == 3U : q || size < 3U;
					if (!shape)
						continue;
					for (unsigned opcode = 8; opcode <= 10; ++opcode) {
						if (opcode == 10U && alternate)
							continue;
						for (const auto registers : register_cases)
							harness.require_differential(two_register(scalar, q,
								alternate, size, opcode, registers),
								test::state(opcode + size));
					}
				}
		}
}

void accumulating_saturation_and_qc()
{
	test::Harness harness;
	auto initial = test::state();
	initial.vectors[0].bytes.fill(std::byte{});
	initial.vectors[1].bytes.fill(std::byte{});
	initial.fpsr = 1U;

	initial.vectors[0].bytes[0] = std::byte{0x7f};
	initial.vectors[1].bytes[0] = std::byte{1};
	const auto suqadd = two_register(false, false, false, 0, 3,
		Registers{0, 1});
	const auto signed_result = harness.run_interpreter(suqadd, initial);
	test::require(signed_result.vectors[0].bytes[0] == std::byte{0x7f});
	test::require((signed_result.fpsr & (1U << 27)) != 0);
	test::require((signed_result.fpsr & 1U) != 0);
	harness.require_differential(suqadd, initial);

	initial.vectors[0].bytes[0] = std::byte{0};
	initial.vectors[1].bytes[0] = std::byte{0xff};
	initial.fpsr = 0;
	const auto usqadd = two_register(false, false, true, 0, 3,
		Registers{0, 1});
	const auto unsigned_result = harness.run_interpreter(usqadd, initial);
	test::require(unsigned_result.vectors[0].bytes[0] == std::byte{0});
	test::require((unsigned_result.fpsr & (1U << 27)) != 0);
	harness.require_differential(usqadd, initial);
}

void unary_integer_edges()
{
	test::Harness harness;
	auto initial = test::state();
	initial.vectors[1].bytes.fill(std::byte{});
	initial.vectors[1].bytes[0] = std::byte{0x80};
	initial.vectors[1].bytes[1] = std::byte{0xff};
	initial.vectors[1].bytes[2] = std::byte{0};
	initial.vectors[1].bytes[3] = std::byte{1};

	for (const bool negate : {false, true}) {
		const auto saturated = two_register(false, false, negate, 0, 7,
			Registers{0, 1});
		const auto result = harness.run_interpreter(saturated, initial);
		test::require(result.vectors[0].bytes[0] == std::byte{0x7f});
		test::require((result.fpsr & (1U << 27)) != 0);
		harness.require_differential(saturated, initial);

		const auto wrapping = two_register(false, false, negate, 0, 11,
			Registers{0, 1});
		const auto wrapped = harness.run_interpreter(wrapping, initial);
		test::require(wrapped.vectors[0].bytes[0] == std::byte{0x80});
		harness.require_differential(wrapping, initial);
	}

	const auto cls = harness.run_interpreter(two_register(false, false, false,
		0, 4, Registers{0, 1}), initial);
	const auto clz = harness.run_interpreter(two_register(false, false, true,
		0, 4, Registers{0, 1}), initial);
	test::require(cls.vectors[0].bytes[1] == std::byte{7});
	test::require(cls.vectors[0].bytes[2] == std::byte{7});
	test::require(clz.vectors[0].bytes[1] == std::byte{0});
	test::require(clz.vectors[0].bytes[2] == std::byte{8});
}

void pairwise_narrow_and_shift_edges()
{
	test::Harness harness;
	auto initial = test::state();
	initial.vectors[0].bytes.fill(std::byte{0x5a});
	initial.vectors[1].bytes.fill(std::byte{});
	initial.vectors[1].bytes[0] = std::byte{0x80};
	initial.vectors[1].bytes[1] = std::byte{0x7f};

	const auto signed_pair = harness.run_interpreter(two_register(false, false,
		false, 0, 2, Registers{0, 1}), initial);
	const auto unsigned_pair = harness.run_interpreter(two_register(false, false,
		true, 0, 2, Registers{0, 1}), initial);
	test::require(read_lane(signed_pair, 0, 0, 2) == 0xffffU);
	test::require(read_lane(unsigned_pair, 0, 0, 2) == 0x00ffU);

	write_lane(initial, 1, 0, 2, 0x0080U);
	write_lane(initial, 1, 2, 2, 0xff7fU);
	const auto sqxtn = two_register(false, false, false, 0, 20,
		Registers{0, 1});
	const auto narrowed = harness.run_interpreter(sqxtn, initial);
	test::require(narrowed.vectors[0].bytes[0] == std::byte{0x7f});
	test::require(narrowed.vectors[0].bytes[1] == std::byte{0x80});
	test::require((narrowed.fpsr & (1U << 27)) != 0);
	harness.require_differential(sqxtn, initial);

	const auto old_low = initial.vectors[0].bytes;
	const auto upper = two_register(false, true, false, 0, 18,
		Registers{0, 1}); // XTN2
	const auto upper_result = harness.run_interpreter(upper, initial);
	for (unsigned byte = 0; byte < 8; ++byte)
		test::require(upper_result.vectors[0].bytes[byte] == old_low[byte]);
	harness.require_differential(upper, initial);

	initial.vectors[1].bytes.fill(std::byte{});
	initial.vectors[1].bytes[0] = std::byte{0x12};
	initial.vectors[1].bytes[8] = std::byte{0x34};
	const auto lower_shift = harness.run_interpreter(two_register(false, false,
		true, 0, 19, Registers{0, 1}), initial);
	const auto upper_shift = harness.run_interpreter(two_register(false, true,
		true, 0, 19, Registers{0, 1}), initial);
	test::require(read_lane(lower_shift, 0, 0, 2) == 0x1200U);
	test::require(read_lane(upper_shift, 0, 0, 2) == 0x3400U);
}

void bit_reverse_and_compare_known_answers()
{
	test::Harness harness;
	auto initial = test::state();
	initial.vectors[1].bytes.fill(std::byte{});
	initial.vectors[1].bytes[0] = std::byte{0x81};
	initial.vectors[1].bytes[1] = std::byte{0x0f};
	initial.vectors[1].bytes[2] = std::byte{0xf0};
	initial.vectors[1].bytes[3] = std::byte{0x55};

	const auto reversed = harness.run_interpreter(two_register(false, false,
		true, 1, 5, Registers{0, 1}), initial);
	const auto counted = harness.run_interpreter(two_register(false, false,
		false, 0, 5, Registers{0, 1}), initial);
	test::require(reversed.vectors[0].bytes[0] == std::byte{0x81});
	test::require(reversed.vectors[0].bytes[1] == std::byte{0xf0});
	test::require(counted.vectors[0].bytes[0] == std::byte{2});
	test::require(counted.vectors[0].bytes[1] == std::byte{4});

	initial.vectors[1].bytes[0] = std::byte{0xff};
	initial.vectors[1].bytes[1] = std::byte{0};
	initial.vectors[1].bytes[2] = std::byte{1};
	constexpr std::array expected{
		std::array{false, false, true},  // CMGT
		std::array{false, true, true},   // CMGE
		std::array{false, true, false},  // CMEQ
		std::array{true, true, false},   // CMLE
		std::array{true, false, false},  // CMLT
	};
	for (unsigned operation = 0; operation < expected.size(); ++operation) {
		const unsigned opcode = operation < 2 ? 8U :
			operation < 4 ? 9U : 10U;
		const bool alternate = operation == 1U || operation == 3U;
		const auto result = harness.run_interpreter(two_register(false, false,
			alternate, 0, opcode, Registers{0, 1}), initial);
		for (unsigned lane = 0; lane < 3; ++lane)
			test::require(result.vectors[0].bytes[lane] ==
				(expected[operation][lane] ? std::byte{0xff} : std::byte{}));
	}
}

} // namespace

int main()
{
	every_shape_operation_and_alias();
	accumulating_saturation_and_qc();
	unary_integer_edges();
	pairwise_narrow_and_shift_edges();
	bit_reverse_and_compare_known_answers();
	return 0;
}
