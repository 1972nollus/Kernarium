// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <array>

namespace test = arm64_simd_test;

namespace {

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

constexpr std::array register_cases{
	Registers{0, 1, 2}, Registers{1, 1, 2},
	Registers{2, 1, 2}, Registers{1, 1, 1},
	Registers{31, 30, 29},
};

[[nodiscard]] constexpr std::uint32_t widening(bool scalar, bool upper,
	bool unsigned_values, unsigned size, unsigned operation,
	Registers registers)
{
	return (scalar ? 0x5e20'0000U : 0x0e20'0000U) |
		(!scalar && upper ? 0x4000'0000U : 0U) |
		(unsigned_values ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) | ((operation & 15U) << 12) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) | (registers.destination & 31U);
}

[[nodiscard]] constexpr bool valid(bool scalar, bool unsigned_values,
	unsigned size, unsigned operation)
{
	if (size >= 3U)
		return false;
	const bool saturating = operation == 9U || operation == 11U ||
		operation == 13U;
	if (scalar)
		return saturating && !unsigned_values && size != 0U;
	if (saturating)
		return !unsigned_values && size != 0U;
	return operation <= 3U || operation == 5U || operation == 7U ||
		operation == 8U || operation == 10U || operation == 12U;
}

[[nodiscard]] constexpr std::uint32_t high_narrow(bool upper, bool rounding,
	bool subtract, unsigned size, Registers registers)
{
	return 0x0e20'4000U | (upper ? 0x4000'0000U : 0U) |
		(rounding ? 0x2000'0000U : 0U) | ((size & 3U) << 22) |
		(subtract ? 0x2000U : 0U) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) | (registers.destination & 31U);
}

void all_operations_sizes_halves_and_aliases()
{
	test::Harness harness;
	for (const bool scalar : {false, true}) {
		for (const bool upper : {false, true}) {
			if (scalar && upper)
				continue;
			for (const bool unsigned_values : {false, true})
				for (unsigned size = 0; size < 3; ++size)
					for (unsigned operation = 0; operation < 14; ++operation) {
						if (!valid(scalar, unsigned_values, size, operation))
							continue;
						for (const auto registers : register_cases)
							for (unsigned seed = 0; seed < 2; ++seed)
								harness.require_differential(widening(
									scalar, upper, unsigned_values, size,
									operation, registers),
									test::state(seed + operation * 5U + size));
					}
		}
	}

	for (const bool upper : {false, true})
		for (const bool rounding : {false, true})
			for (const bool subtract : {false, true})
				for (unsigned size = 0; size < 3; ++size)
					for (const auto registers : register_cases)
						harness.require_differential(high_narrow(upper,
							rounding, subtract, size, registers),
							test::state(size + 31U));
}

void scalar_uses_low_source_lanes_and_sets_qc()
{
	test::Harness harness;
	auto initial = test::state();
	initial.vectors[1].bytes.fill(std::byte{0});
	initial.vectors[2].bytes.fill(std::byte{0});
	initial.vectors[1].bytes[0] = std::byte{2};
	initial.vectors[2].bytes[0] = std::byte{3};
	for (unsigned byte = 8; byte < 16; ++byte) {
		initial.vectors[1].bytes[byte] = std::byte{0x7f};
		initial.vectors[2].bytes[byte] = std::byte{0x7f};
	}
	const auto multiply = widening(true, false, false, 1, 13,
		Registers{0, 1, 2});
	const auto result = harness.run_interpreter(multiply, initial);
	test::require(result.vectors[0].bytes[0] == std::byte{12});
	for (unsigned byte = 1; byte < 16; ++byte)
		test::require(result.vectors[0].bytes[byte] == std::byte{0});
	harness.require_differential(multiply, initial);

	/* (-32768 * -32768 * 2) exceeds signed 32-bit and must set QC. */
	initial.vectors[1].bytes[0] = std::byte{0};
	initial.vectors[1].bytes[1] = std::byte{0x80};
	initial.vectors[2].bytes[0] = std::byte{0};
	initial.vectors[2].bytes[1] = std::byte{0x80};
	initial.fpsr = 1U;
	const auto saturated = harness.run_interpreter(multiply, initial);
	test::require((saturated.fpsr & (1U << 27)) != 0);
	test::require((saturated.fpsr & 1U) != 0);
	harness.require_differential(multiply, initial);
}

void upper_high_narrow_preserves_low_destination()
{
	test::Harness harness;
	auto initial = test::state(9);
	const auto before = initial.vectors[0].bytes;
	const auto instruction = high_narrow(true, true, true, 2,
		Registers{0, 1, 2});
	const auto result = harness.run_interpreter(instruction, initial);
	for (unsigned byte = 0; byte < 8; ++byte)
		test::require(result.vectors[0].bytes[byte] == before[byte]);
	harness.require_differential(instruction, initial);
}

} // namespace

int main()
{
	all_operations_sizes_halves_and_aliases();
	scalar_uses_low_source_lanes_and_sets_qc();
	upper_high_narrow_preserves_low_destination();
	return 0;
}
