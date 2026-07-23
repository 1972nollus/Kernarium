// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

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

[[nodiscard]] constexpr std::uint32_t pmull(bool high, unsigned size,
	Registers registers)
{
	return 0x0e20'e000U | (high ? 0x4000'0000U : 0U) |
		((size & 3U) << 22) | ((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint16_t multiply8(
	std::uint8_t left, std::uint8_t right)
{
	std::uint16_t result = 0;
	for (unsigned bit = 0; bit < 8; ++bit)
		if ((right & (1U << bit)) != 0)
			result ^= static_cast<std::uint16_t>(left) << bit;
	return result;
}

[[nodiscard]] unsigned __int128 multiply64(
	std::uint64_t left, std::uint64_t right)
{
	unsigned __int128 result = 0;
	for (unsigned bit = 0; bit < 64; ++bit)
		if ((right & (std::uint64_t{1} << bit)) != 0)
			result ^= static_cast<unsigned __int128>(left) << bit;
	return result;
}

[[nodiscard]] std::uint64_t lane64(
	const test::arm64::VectorRegister &vector, unsigned lane)
{
	std::uint64_t result;
	std::memcpy(&result, vector.bytes.data() + lane * sizeof(result),
		sizeof(result));
	return result;
}

void every_form_and_alias()
{
	test::Harness harness;
	for (const bool high : {false, true})
		for (const unsigned size : {0U, 3U})
			for (const auto registers : register_cases)
				for (unsigned seed = 0; seed < 32; ++seed) {
					const auto initial = test::state(
						seed + size * 13U + high * 29U);
					test::arm64::VectorRegister expected{};
					if (size == 0U) {
						const unsigned offset = high ? 8U : 0U;
						for (unsigned lane = 0; lane < 8; ++lane) {
							const auto left = std::to_integer<std::uint8_t>(
								initial.vectors[registers.left]
									.bytes[offset + lane]);
							const auto right = std::to_integer<std::uint8_t>(
								initial.vectors[registers.right]
									.bytes[offset + lane]);
							const auto product = multiply8(left, right);
							std::memcpy(expected.bytes.data() + lane * 2U,
								&product, sizeof(product));
						}
					} else {
						const auto product = multiply64(
							lane64(initial.vectors[registers.left], high ? 1U : 0U),
							lane64(initial.vectors[registers.right], high ? 1U : 0U));
						const auto low = static_cast<std::uint64_t>(product);
						const auto upper = static_cast<std::uint64_t>(product >> 64);
						std::memcpy(expected.bytes.data(), &low, sizeof(low));
						std::memcpy(expected.bytes.data() + 8, &upper, sizeof(upper));
					}
					const auto instruction = pmull(high, size, registers);
					const auto result = harness.run_interpreter(instruction, initial);
					test::require(result.vectors[registers.destination].bytes ==
						expected.bytes);
					harness.require_differential(instruction, initial);
				}
}

} // namespace

int main()
{
	every_form_and_alias();
	return 0;
}
