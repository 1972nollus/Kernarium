// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <bit>

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t three(
	unsigned operation, unsigned destination, unsigned first, unsigned second)
{
	return 0x5e00'0000U | (second << 16) | (operation << 12) |
		(first << 5) | destination;
}

[[nodiscard]] constexpr std::uint32_t two(
	unsigned operation, unsigned destination, unsigned source)
{
	return 0x5e28'0800U | (operation << 12) | (source << 5) | destination;
}

[[nodiscard]] std::array<std::uint32_t, 4> words(
	const uml::backend::arm64::VectorRegister &source)
{
	std::array<std::uint32_t, 4> result{};
	for (unsigned lane = 0; lane < 4; ++lane)
		for (unsigned byte = 0; byte < 4; ++byte)
			result[lane] |= std::to_integer<std::uint32_t>(
				source.bytes[lane * 4 + byte]) << (byte * 8U);
	return result;
}

[[nodiscard]] constexpr std::uint32_t choose(
	std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
	return ((y ^ z) & x) ^ z;
}

[[nodiscard]] constexpr std::uint32_t majority(
	std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
	return (x & y) | ((x | y) & z);
}

[[nodiscard]] std::array<std::uint32_t, 4> hash(
	std::array<std::uint32_t, 4> x, std::array<std::uint32_t, 4> y,
	const std::array<std::uint32_t, 4> &schedule, bool part1)
{
	for (unsigned round = 0; round < 4; ++round) {
		const auto sigma0 = std::rotr(x[0], 2) ^ std::rotr(x[0], 13) ^
			std::rotr(x[0], 22);
		const auto sigma1 = std::rotr(y[0], 6) ^ std::rotr(y[0], 11) ^
			std::rotr(y[0], 25);
		const std::uint32_t temporary = y[3] + sigma1 +
			choose(y[0], y[1], y[2]) + schedule[round];
		x[3] += temporary;
		y[3] = temporary + sigma0 + majority(x[0], x[1], x[2]);
		const auto old_x = x;
		const auto old_y = y;
		x = {old_y[3], old_x[0], old_x[1], old_x[2]};
		y = {old_x[3], old_y[0], old_y[1], old_y[2]};
	}
	return part1 ? x : y;
}

[[nodiscard]] std::array<std::uint32_t, 4> schedule0(
	const std::array<std::uint32_t, 4> &destination,
	const std::array<std::uint32_t, 4> &source)
{
	const std::array temporary{destination[1], destination[2],
		destination[3], source[0]};
	std::array<std::uint32_t, 4> result{};
	for (unsigned lane = 0; lane < 4; ++lane) {
		const auto value = temporary[lane];
		result[lane] = destination[lane] + (std::rotr(value, 7) ^
			std::rotr(value, 18) ^ (value >> 3));
	}
	return result;
}

[[nodiscard]] std::array<std::uint32_t, 4> schedule1(
	const std::array<std::uint32_t, 4> &destination,
	const std::array<std::uint32_t, 4> &first,
	const std::array<std::uint32_t, 4> &second)
{
	const std::array temporary{first[1], first[2], first[3], second[0]};
	std::array<std::uint32_t, 4> result{};
	for (unsigned lane = 0; lane < 2; ++lane) {
		const auto value = second[lane + 2];
		result[lane] = destination[lane] + temporary[lane] +
			(std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10));
	}
	for (unsigned lane = 2; lane < 4; ++lane) {
		const auto value = result[lane - 2];
		result[lane] = destination[lane] + temporary[lane] +
			(std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10));
	}
	return result;
}

void hash_and_schedule_forms_include_all_alias_patterns()
{
	struct Registers final { unsigned destination; unsigned first; unsigned second; };
	constexpr std::array cases{
		Registers{28, 29, 30}, Registers{28, 28, 30},
		Registers{28, 29, 28}, Registers{28, 29, 29},
		Registers{28, 28, 28}};
	test::Harness harness;
	for (unsigned operation = 4; operation < 7; ++operation)
		for (const auto registers : cases) {
			auto initial = test::state(operation + registers.first + 33U);
			const auto destination = words(initial.vectors[registers.destination]);
			const auto first = words(initial.vectors[registers.first]);
			const auto second = words(initial.vectors[registers.second]);
			const auto expected = operation == 4U ?
				hash(destination, first, second, true) : operation == 5U ?
				hash(first, destination, second, false) :
				schedule1(destination, first, second);
			const auto encoded = three(operation, registers.destination,
				registers.first, registers.second);
			const auto interpreted = harness.run_interpreter(encoded, initial);
			test::require(words(interpreted.vectors[registers.destination]) == expected);
			harness.require_differential(encoded, initial);
		}
}

void schedule_zero_includes_source_destination_aliasing()
{
	test::Harness harness;
	for (const bool alias : {false, true}) {
		const unsigned destination = 30;
		const unsigned source = alias ? destination : 31U;
		auto initial = test::state(alias + 102U);
		const auto old_destination = words(initial.vectors[destination]);
		const auto old_source = words(initial.vectors[source]);
		const auto expected = schedule0(old_destination, old_source);
		const auto encoded = two(2U, destination, source);
		const auto interpreted = harness.run_interpreter(encoded, initial);
		test::require(words(interpreted.vectors[destination]) == expected);
		harness.require_differential(encoded, initial);
	}
}

} // namespace

int main()
{
	hash_and_schedule_forms_include_all_alias_patterns();
	schedule_zero_includes_source_destination_aliasing();
	return 0;
}
