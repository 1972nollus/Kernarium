// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

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

void set_words(uml::backend::arm64::VectorRegister &destination,
	const std::array<std::uint32_t, 4> &values)
{
	for (unsigned lane = 0; lane < 4; ++lane)
		for (unsigned byte = 0; byte < 4; ++byte)
			destination.bytes[lane * 4 + byte] =
				static_cast<std::byte>(values[lane] >> (byte * 8U));
}

[[nodiscard]] constexpr std::uint32_t key_instruction(
	unsigned destination, unsigned input, unsigned constant)
{
	return 0xce60'c800U | (constant << 16) | (input << 5) | destination;
}

[[nodiscard]] constexpr std::uint32_t round_instruction(
	unsigned destination, unsigned keys)
{
	return 0xcec0'8400U | (keys << 5) | destination;
}

void standard_key_schedule_and_round_vectors()
{
	test::Harness harness;
	auto key_state = test::state(0);
	set_words(key_state.vectors[1], {0xa292'ffa1U, 0xdf01'febfU,
		0x99a1'2b0fU, 0xc424'10ccU});
	set_words(key_state.vectors[2], {0x0007'0e15U, 0x1c23'2a31U,
		0x383f'464dU, 0x545b'6269U});
	constexpr std::array expected_keys{0xf121'86f9U, 0x4166'2b61U,
		0x5a6a'b19aU, 0x7ba9'2077U};
	const auto key_encoded = key_instruction(0, 1, 2);
	const auto keys = harness.run_interpreter(key_encoded, key_state);
	test::require(words(keys.vectors[0]) == expected_keys);
	harness.require_differential(key_encoded, key_state);

	auto round_state = test::state(0);
	set_words(round_state.vectors[0], {0x0123'4567U, 0x89ab'cdefU,
		0xfedc'ba98U, 0x7654'3210U});
	set_words(round_state.vectors[1], expected_keys);
	constexpr std::array expected_rounds{0x27fa'd345U, 0xa18b'4cb2U,
		0x11c1'e22aU, 0xcc13'e2eeU};
	const auto round_encoded = round_instruction(0, 1);
	const auto rounds = harness.run_interpreter(round_encoded, round_state);
	test::require(words(rounds.vectors[0]) == expected_rounds);
	harness.require_differential(round_encoded, round_state);
}

void all_source_destination_alias_patterns_match_native_gadgets()
{
	test::Harness harness;
	struct Registers final { unsigned destination; unsigned input; unsigned constant; };
	constexpr std::array cases{Registers{28,29,30}, Registers{28,28,30},
		Registers{28,29,28}, Registers{28,29,29}, Registers{28,28,28}};
	for (const auto registers : cases) {
		const auto encoded = key_instruction(registers.destination,
			registers.input, registers.constant);
		harness.require_differential(encoded,
			test::state(registers.destination + registers.input));
	}
	for (const bool alias : {false, true}) {
		const auto encoded = round_instruction(30, alias ? 30U : 31U);
		harness.require_differential(encoded, test::state(alias + 144U));
	}
}

} // namespace

int main()
{
	standard_key_schedule_and_round_vectors();
	all_source_destination_alias_patterns_match_native_gadgets();
	return 0;
}
