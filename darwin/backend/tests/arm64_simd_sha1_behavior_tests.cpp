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
	std::array<std::uint32_t, 4> x, std::uint32_t y,
	const std::array<std::uint32_t, 4> &schedule, unsigned operation)
{
	for (unsigned round = 0; round < 4; ++round) {
		const auto function = operation == 0U ? choose(x[1], x[2], x[3]) :
			operation == 1U ? x[1] ^ x[2] ^ x[3] :
			majority(x[1], x[2], x[3]);
		const std::uint32_t next = y + std::rotl(x[0], 5) + function +
			schedule[round];
		x[1] = std::rotl(x[1], 30);
		y = x[3];
		x = {next, x[0], x[1], x[2]};
	}
	return x;
}

[[nodiscard]] std::array<std::uint32_t, 4> schedule0(
	const std::array<std::uint32_t, 4> &destination,
	const std::array<std::uint32_t, 4> &first,
	const std::array<std::uint32_t, 4> &second)
{
	return {destination[2] ^ destination[0] ^ second[0],
		destination[3] ^ destination[1] ^ second[1],
		first[0] ^ destination[2] ^ second[2],
		first[1] ^ destination[3] ^ second[3]};
}

[[nodiscard]] std::array<std::uint32_t, 4> schedule1(
	const std::array<std::uint32_t, 4> &destination,
	const std::array<std::uint32_t, 4> &source)
{
	const std::array temporary{destination[0] ^ source[1],
		destination[1] ^ source[2], destination[2] ^ source[3],
		destination[3]};
	return {std::rotl(temporary[0], 1), std::rotl(temporary[1], 1),
		std::rotl(temporary[2], 1),
		std::rotl(temporary[3], 1) ^ std::rotl(temporary[0], 2)};
}

void hash_and_schedule_forms_include_all_alias_patterns()
{
	struct Registers final { unsigned destination; unsigned first; unsigned second; };
	constexpr std::array cases{
		Registers{28, 29, 30}, Registers{28, 28, 30},
		Registers{28, 29, 28}, Registers{28, 29, 29},
		Registers{28, 28, 28}};
	test::Harness harness;
	for (unsigned operation = 0; operation < 4; ++operation)
		for (const auto registers : cases) {
			auto initial = test::state(operation + registers.first);
			const auto destination = words(initial.vectors[registers.destination]);
			const auto first = words(initial.vectors[registers.first]);
			const auto second = words(initial.vectors[registers.second]);
			const auto expected = operation < 3U ?
				hash(destination, first[0], second, operation) :
				schedule0(destination, first, second);
			const auto encoded = three(operation, registers.destination,
				registers.first, registers.second);
			const auto interpreted = harness.run_interpreter(encoded, initial);
			test::require(words(interpreted.vectors[registers.destination]) == expected);
			harness.require_differential(encoded, initial);
		}
}

void unary_forms_include_source_destination_aliasing()
{
	test::Harness harness;
	for (unsigned operation = 0; operation < 2; ++operation)
		for (const bool alias : {false, true}) {
			const unsigned destination = 30;
			const unsigned source = alias ? destination : 31U;
			auto initial = test::state(operation + alias + 71U);
			const auto old_destination = words(initial.vectors[destination]);
			const auto old_source = words(initial.vectors[source]);
			const std::array<std::uint32_t, 4> expected = operation == 0U ?
				std::array<std::uint32_t, 4>{std::rotl(old_source[0], 30), 0, 0, 0} :
				schedule1(old_destination, old_source);
			const auto encoded = two(operation, destination, source);
			const auto interpreted = harness.run_interpreter(encoded, initial);
			test::require(words(interpreted.vectors[destination]) == expected);
			harness.require_differential(encoded, initial);
		}
}

} // namespace

int main()
{
	hash_and_schedule_forms_include_all_alias_patterns();
	unary_forms_include_source_destination_aliasing();
	return 0;
}
