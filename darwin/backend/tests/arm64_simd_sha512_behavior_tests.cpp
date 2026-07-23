// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <bit>

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t three(
	std::uint32_t operation, unsigned destination, unsigned first,
	unsigned second)
{
	return operation | (second << 16) | (first << 5) | destination;
}

[[nodiscard]] constexpr std::uint32_t schedule_zero(
	unsigned destination, unsigned source)
{
	return 0xcec0'8000U | (source << 5) | destination;
}

[[nodiscard]] std::array<std::uint64_t, 2> words(
	const uml::backend::arm64::VectorRegister &source)
{
	std::array<std::uint64_t, 2> result{};
	for (unsigned lane = 0; lane < 2; ++lane)
		for (unsigned byte = 0; byte < 8; ++byte)
			result[lane] |= std::to_integer<std::uint64_t>(
				source.bytes[lane * 8 + byte]) << (byte * 8U);
	return result;
}

[[nodiscard]] std::array<std::uint64_t, 2> hash_part1(
	const std::array<std::uint64_t, 2> &w,
	const std::array<std::uint64_t, 2> &x,
	const std::array<std::uint64_t, 2> &y)
{
	const auto sigma1 = [](std::uint64_t value) {
		return std::rotr(value, 14) ^ std::rotr(value, 18) ^
			std::rotr(value, 41);
	};
	std::array<std::uint64_t, 2> result{};
	result[1] = ((y[1] & x[0]) ^ (~y[1] & x[1])) + sigma1(y[1]) + w[1];
	const auto temporary = result[1] + y[0];
	result[0] = ((temporary & y[1]) ^ (~temporary & x[0])) +
		sigma1(temporary) + w[0];
	return result;
}

[[nodiscard]] std::array<std::uint64_t, 2> hash_part2(
	const std::array<std::uint64_t, 2> &w,
	const std::array<std::uint64_t, 2> &x,
	const std::array<std::uint64_t, 2> &y)
{
	const auto sigma0 = [](std::uint64_t value) {
		return std::rotr(value, 28) ^ std::rotr(value, 34) ^
			std::rotr(value, 39);
	};
	const auto majority = [](std::uint64_t a, std::uint64_t b,
		std::uint64_t c) { return (a & b) | ((a | b) & c); };
	std::array<std::uint64_t, 2> result{};
	result[1] = majority(x[0], y[1], y[0]) + sigma0(y[0]) + w[1];
	result[0] = majority(result[1], y[0], y[1]) + sigma0(result[1]) + w[0];
	return result;
}

[[nodiscard]] std::array<std::uint64_t, 2> schedule1(
	const std::array<std::uint64_t, 2> &w,
	const std::array<std::uint64_t, 2> &x,
	const std::array<std::uint64_t, 2> &y)
{
	std::array<std::uint64_t, 2> result{};
	for (unsigned lane = 0; lane < 2; ++lane) {
		const auto sigma1 = std::rotr(x[lane], 19) ^ std::rotr(x[lane], 61) ^
			(x[lane] >> 6);
		result[lane] = w[lane] + sigma1 + y[lane];
	}
	return result;
}

void three_register_forms_include_all_alias_patterns()
{
	struct Registers final { unsigned destination; unsigned first; unsigned second; };
	constexpr std::array cases{
		Registers{28, 29, 30}, Registers{28, 28, 30},
		Registers{28, 29, 28}, Registers{28, 29, 29},
		Registers{28, 28, 28}};
	constexpr std::array operations{0xce60'8000U, 0xce60'8400U, 0xce60'8800U};
	test::Harness harness;
	for (const auto operation : operations)
		for (const auto registers : cases) {
			auto initial = test::state(operation + registers.first);
			const auto destination = words(initial.vectors[registers.destination]);
			const auto first = words(initial.vectors[registers.first]);
			const auto second = words(initial.vectors[registers.second]);
			const auto expected = operation == operations[0] ?
				hash_part1(destination, first, second) :
				operation == operations[1] ? hash_part2(destination, first, second) :
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
		auto initial = test::state(alias + 207U);
		const auto w = words(initial.vectors[destination]);
		const auto x = words(initial.vectors[source]);
		const auto sigma0 = [](std::uint64_t value) {
			return std::rotr(value, 1) ^ std::rotr(value, 8) ^ (value >> 7);
		};
		const std::array expected{w[0] + sigma0(w[1]), w[1] + sigma0(x[0])};
		const auto encoded = schedule_zero(destination, source);
		const auto interpreted = harness.run_interpreter(encoded, initial);
		test::require(words(interpreted.vectors[destination]) == expected);
		harness.require_differential(encoded, initial);
	}
}

} // namespace

int main()
{
	three_register_forms_include_all_alias_patterns();
	schedule_zero_includes_source_destination_aliasing();
	return 0;
}
