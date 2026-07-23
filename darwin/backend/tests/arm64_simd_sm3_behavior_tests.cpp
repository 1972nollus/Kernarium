// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <bit>

namespace test = arm64_simd_test;

namespace {

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

[[nodiscard]] constexpr std::uint32_t three(std::uint32_t operation,
	unsigned destination, unsigned first, unsigned second)
{
	return operation | (second << 16) | (first << 5) | destination;
}

[[nodiscard]] constexpr std::uint32_t four(unsigned destination,
	unsigned first, unsigned second, unsigned third)
{
	return 0xce40'0000U | (second << 16) | (third << 10) |
		(first << 5) | destination;
}

[[nodiscard]] constexpr std::uint32_t indexed(unsigned operation,
	unsigned index, unsigned destination, unsigned first, unsigned second)
{
	return 0xce40'8000U | (second << 16) | (index << 12) |
		(operation << 10) | (first << 5) | destination;
}

[[nodiscard]] std::array<std::uint32_t, 4> partw1(
	const std::array<std::uint32_t, 4> &destination,
	const std::array<std::uint32_t, 4> &first,
	const std::array<std::uint32_t, 4> &second)
{
	std::array<std::uint32_t, 4> result{};
	for (unsigned lane = 0; lane < 3; ++lane) {
		const auto value = destination[lane] ^ first[lane] ^
			std::rotl(second[lane + 1], 15);
		result[lane] = value ^ std::rotl(value, 15) ^ std::rotl(value, 23);
	}
	const auto value = destination[3] ^ first[3] ^ std::rotl(result[0], 15);
	result[3] = value ^ std::rotl(value, 15) ^ std::rotl(value, 23);
	return result;
}

[[nodiscard]] std::array<std::uint32_t, 4> partw2(
	const std::array<std::uint32_t, 4> &destination,
	const std::array<std::uint32_t, 4> &first,
	const std::array<std::uint32_t, 4> &second)
{
	std::array<std::uint32_t, 4> result{};
	std::array<std::uint32_t, 4> temporary{};
	for (unsigned lane = 0; lane < 4; ++lane) {
		temporary[lane] = first[lane] ^ std::rotl(second[lane], 7);
		result[lane] = destination[lane] ^ temporary[lane];
	}
	auto correction = std::rotl(temporary[0], 15);
	correction ^= std::rotl(correction, 15) ^ std::rotl(correction, 23);
	result[3] ^= correction;
	return result;
}

[[nodiscard]] std::array<std::uint32_t, 4> tt(
	const std::array<std::uint32_t, 4> &destination,
	const std::array<std::uint32_t, 4> &first,
	const std::array<std::uint32_t, 4> &second,
	unsigned operation, unsigned index)
{
	std::uint32_t value;
	if (operation == 0U || operation == 2U)
		value = destination[1] ^ destination[3] ^ destination[2];
	else if (operation == 1U)
		value = (destination[3] & destination[1]) |
			(destination[3] & destination[2]) |
			(destination[1] & destination[2]);
	else
		value = (destination[3] & destination[2]) |
			(~destination[3] & destination[1]);
	value += destination[0] + second[index] + (operation < 2U ?
		(first[3] ^ std::rotl(destination[3], 12)) : first[3]);
	if (operation >= 2U)
		value ^= std::rotl(value, 9) ^ std::rotl(value, 17);
	return {destination[1], std::rotl(destination[2], operation < 2U ? 9 : 19),
		destination[3], value};
}

void partw_forms_and_aliasing()
{
	struct Registers final { unsigned destination; unsigned first; unsigned second; };
	constexpr std::array cases{Registers{28,29,30}, Registers{28,28,30},
		Registers{28,29,28}, Registers{28,29,29}, Registers{28,28,28}};
	test::Harness harness;
	for (const bool second_part : {false, true})
		for (const auto registers : cases) {
			auto initial = test::state(second_part + registers.first + 19U);
			const auto destination = words(initial.vectors[registers.destination]);
			const auto first = words(initial.vectors[registers.first]);
			const auto second = words(initial.vectors[registers.second]);
			const auto expected = second_part ? partw2(destination, first, second) :
				partw1(destination, first, second);
			const auto encoded = three(second_part ? 0xce60'c400U : 0xce60'c000U,
				registers.destination, registers.first, registers.second);
			const auto interpreted = harness.run_interpreter(encoded, initial);
			test::require(words(interpreted.vectors[registers.destination]) == expected);
			harness.require_differential(encoded, initial);
		}
}

void ss1_and_tt_forms()
{
	test::Harness harness;
	for (const bool alias : {false, true}) {
		const unsigned destination = 28;
		const unsigned first = alias ? destination : 29U;
		const unsigned second = alias ? destination : 30U;
		const unsigned third = alias ? destination : 31U;
		auto initial = test::state(alias + 89U);
		const auto a = words(initial.vectors[first]);
		const auto b = words(initial.vectors[second]);
		const auto c = words(initial.vectors[third]);
		const std::array expected{0U, 0U, 0U,
			std::rotl(std::rotl(a[3], 12) + b[3] + c[3], 7)};
		const auto encoded = four(destination, first, second, third);
		const auto interpreted = harness.run_interpreter(encoded, initial);
		test::require(words(interpreted.vectors[destination]) == expected);
		harness.require_differential(encoded, initial);
	}
	for (unsigned operation = 0; operation < 4; ++operation)
		for (unsigned index = 0; index < 4; ++index)
			for (const bool alias : {false, true}) {
				const unsigned destination = 28;
				const unsigned first = alias ? destination : 29U;
				const unsigned second = alias ? destination : 30U;
				auto initial = test::state(operation * 7U + index + alias);
				const auto destination_words = words(initial.vectors[destination]);
				const auto first_words = words(initial.vectors[first]);
				const auto second_words = words(initial.vectors[second]);
				const auto expected = tt(destination_words, first_words,
					second_words, operation, index);
				const auto encoded = indexed(operation, index, destination,
					first, second);
				const auto interpreted = harness.run_interpreter(encoded, initial);
				test::require(words(interpreted.vectors[destination]) == expected);
				harness.require_differential(encoded, initial);
			}
}

} // namespace

int main()
{
	partw_forms_and_aliasing();
	ss1_and_tt_forms();
	return 0;
}
