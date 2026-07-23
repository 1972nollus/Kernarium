// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t aes(unsigned operation,
	unsigned destination, unsigned source)
{
	return 0x4e28'0800U | ((operation & 7U) << 12) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint8_t multiply(
	std::uint8_t left, std::uint8_t right)
{
	std::uint8_t result = 0;
	for (unsigned bit = 0; bit < 8; ++bit) {
		if ((right & 1U) != 0)
			result ^= left;
		const bool high = (left & 0x80U) != 0;
		left = static_cast<std::uint8_t>(left << 1);
		if (high)
			left ^= 0x1bU;
		right = static_cast<std::uint8_t>(right >> 1);
	}
	return result;
}

[[nodiscard]] constexpr std::uint8_t rotate_left(std::uint8_t value,
	unsigned amount)
{
	return static_cast<std::uint8_t>((value << amount) |
		(value >> (8U - amount)));
}

[[nodiscard]] constexpr std::uint8_t inverse(std::uint8_t value)
{
	if (value == 0)
		return 0;
	std::uint8_t result = 1;
	std::uint8_t base = value;
	unsigned power = 254;
	while (power != 0) {
		if ((power & 1U) != 0)
			result = multiply(result, base);
		base = multiply(base, base);
		power >>= 1;
	}
	return result;
}

[[nodiscard]] constexpr std::uint8_t substitute(std::uint8_t value)
{
	const auto value_inverse = inverse(value);
	return static_cast<std::uint8_t>(value_inverse ^
		rotate_left(value_inverse, 1) ^ rotate_left(value_inverse, 2) ^
		rotate_left(value_inverse, 3) ^ rotate_left(value_inverse, 4) ^ 0x63U);
}

[[nodiscard]] constexpr std::uint8_t inverse_substitute(std::uint8_t value)
{
	for (unsigned candidate = 0; candidate < 256; ++candidate)
		if (substitute(static_cast<std::uint8_t>(candidate)) == value)
			return static_cast<std::uint8_t>(candidate);
	return 0;
}

[[nodiscard]] std::array<std::uint8_t, 16> bytes(
	const test::arm64::VectorRegister &vector)
{
	std::array<std::uint8_t, 16> result{};
	for (unsigned index = 0; index < result.size(); ++index)
		result[index] = std::to_integer<std::uint8_t>(vector.bytes[index]);
	return result;
}

[[nodiscard]] std::array<std::uint8_t, 16> round(
	const std::array<std::uint8_t, 16> &destination,
	const std::array<std::uint8_t, 16> &key, bool decrypt)
{
	std::array<std::uint8_t, 16> input{};
	std::array<std::uint8_t, 16> result{};
	for (unsigned index = 0; index < 16; ++index)
		input[index] = destination[index] ^ key[index];
	for (unsigned column = 0; column < 4; ++column)
		for (unsigned row = 0; row < 4; ++row) {
			const unsigned source_column = decrypt ?
				(column + 4U - row) & 3U : (column + row) & 3U;
			const auto value = input[source_column * 4U + row];
			result[column * 4U + row] = decrypt ?
				inverse_substitute(value) : substitute(value);
		}
	return result;
}

[[nodiscard]] std::array<std::uint8_t, 16> mix(
	const std::array<std::uint8_t, 16> &input, bool inverse_mix)
{
	std::array<std::uint8_t, 16> result{};
	for (unsigned column = 0; column < 4; ++column) {
		const auto *value = input.data() + column * 4U;
		if (!inverse_mix) {
			result[column * 4U] = multiply(value[0], 2) ^
				multiply(value[1], 3) ^ value[2] ^ value[3];
			result[column * 4U + 1] = value[0] ^ multiply(value[1], 2) ^
				multiply(value[2], 3) ^ value[3];
			result[column * 4U + 2] = value[0] ^ value[1] ^
				multiply(value[2], 2) ^ multiply(value[3], 3);
			result[column * 4U + 3] = multiply(value[0], 3) ^ value[1] ^
				value[2] ^ multiply(value[3], 2);
		} else {
			result[column * 4U] = multiply(value[0], 14) ^
				multiply(value[1], 11) ^ multiply(value[2], 13) ^
				multiply(value[3], 9);
			result[column * 4U + 1] = multiply(value[0], 9) ^
				multiply(value[1], 14) ^ multiply(value[2], 11) ^
				multiply(value[3], 13);
			result[column * 4U + 2] = multiply(value[0], 13) ^
				multiply(value[1], 9) ^ multiply(value[2], 14) ^
				multiply(value[3], 11);
			result[column * 4U + 3] = multiply(value[0], 11) ^
				multiply(value[1], 13) ^ multiply(value[2], 9) ^
				multiply(value[3], 14);
		}
	}
	return result;
}

void every_operation_and_alias()
{
	test::Harness harness;
	for (unsigned operation = 4; operation < 8; ++operation)
		for (const auto registers : {std::array{0U, 1U}, std::array{1U, 1U},
			std::array{31U, 30U}})
			for (unsigned seed = 0; seed < 64; ++seed) {
				auto initial = test::state(seed + operation * 19U);
				const auto destination = bytes(initial.vectors[registers[0]]);
				const auto source = bytes(initial.vectors[registers[1]]);
				const auto expected = operation == 4U ?
					round(destination, source, false) : operation == 5U ?
					round(destination, source, true) :
					mix(source, operation == 7U);
				const auto instruction = aes(operation, registers[0], registers[1]);
				const auto result = harness.run_interpreter(instruction, initial);
				test::require(bytes(result.vectors[registers[0]]) == expected);
				harness.require_differential(instruction, initial);
			}
}

void characteristic_known_answers()
{
	test::Harness harness;
	auto initial = test::state();
	initial.vectors[0].bytes.fill(std::byte{});
	initial.vectors[1].bytes.fill(std::byte{});
	auto encrypted = harness.run_interpreter(aes(4, 0, 1), initial);
	for (const auto value : encrypted.vectors[0].bytes)
		test::require(value == std::byte{0x63});
	auto decrypted = harness.run_interpreter(aes(5, 0, 1), initial);
	for (const auto value : decrypted.vectors[0].bytes)
		test::require(value == std::byte{0x52});

	constexpr std::array<std::uint8_t, 4> input{0xdb, 0x13, 0x53, 0x45};
	constexpr std::array<std::uint8_t, 4> output{0x8e, 0x4d, 0xa1, 0xbc};
	for (unsigned column = 0; column < 4; ++column)
		for (unsigned row = 0; row < 4; ++row)
			initial.vectors[1].bytes[column * 4U + row] =
				static_cast<std::byte>(input[row]);
	const auto mixed = harness.run_interpreter(aes(6, 0, 1), initial);
	for (unsigned column = 0; column < 4; ++column)
		for (unsigned row = 0; row < 4; ++row)
			test::require(mixed.vectors[0].bytes[column * 4U + row] ==
				static_cast<std::byte>(output[row]));
}

} // namespace

int main()
{
	characteristic_known_answers();
	every_operation_and_alias();
	return 0;
}
