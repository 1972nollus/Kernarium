// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_aes_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace test = arm64_simd_aes_test;

namespace {

constexpr std::array operations{
	test::Operation::encrypt_round,
	test::Operation::decrypt_round,
	test::Operation::mix_columns,
	test::Operation::inverse_mix_columns,
};

constexpr std::array register_cases{
	test::Registers{0, 1},
	test::Registers{1, 0},
	test::Registers{1, 1},
	test::Registers{0, 31},
	test::Registers{31, 0},
	test::Registers{31, 30},
	test::Registers{30, 31},
	test::Registers{31, 31},
};

constexpr std::array<std::uint8_t, 16> edge_values{
	0x00, 0xff, 0x01, 0x80, 0x7f, 0x55, 0xaa, 0x1b,
	0x63, 0x52, 0xdb, 0x13, 0x53, 0x45, 0x8e, 0xbc,
};

[[nodiscard]] test::arm64::CpuState edge_state(unsigned pattern)
{
	auto cpu = test::state(pattern + 100U);
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < 16U; ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				edge_values[(pattern * 3U + reg * 5U +
					byte * 7U) % edge_values.size()] ^
				static_cast<std::uint8_t>(
					(reg * 11U + byte * 29U) & 0xffU));
	return cpu;
}

void oracle_known_answers()
{
	std::array<std::byte, 16> zero{};
	std::array<std::byte, 16> one{};
	one[0] = std::byte{1};
	const auto encrypted = test::round_transform(zero, one, false);
	test::require(encrypted[0] == std::byte{0x7c},
		"forward round did not combine operands before substitution");
	for (unsigned byte = 1; byte < encrypted.size(); ++byte)
		test::require(encrypted[byte] == std::byte{0x63},
			"forward zero-byte substitution anchor mismatch");

	const auto decrypted = test::round_transform(zero, one, true);
	test::require(decrypted[0] == std::byte{0x09},
		"inverse round did not combine operands before substitution");
	for (unsigned byte = 1; byte < decrypted.size(); ++byte)
		test::require(decrypted[byte] == std::byte{0x52},
			"inverse zero-byte substitution anchor mismatch");

	constexpr std::array<std::uint8_t, 4> input{
		0xdb, 0x13, 0x53, 0x45,
	};
	constexpr std::array<std::uint8_t, 4> output{
		0x8e, 0x4d, 0xa1, 0xbc,
	};
	std::array<std::byte, 16> columns{};
	for (unsigned column = 0; column < 4U; ++column)
		for (unsigned row = 0; row < 4U; ++row)
			columns[column * 4U + row] =
				static_cast<std::byte>(input[row]);
	const auto mixed = test::column_transform(columns, false);
	for (unsigned column = 0; column < 4U; ++column)
		for (unsigned row = 0; row < 4U; ++row)
			test::require(mixed[column * 4U + row] ==
					static_cast<std::byte>(output[row]),
				"forward column-transform anchor mismatch");
	test::require(test::column_transform(mixed, true) == columns,
		"forward/inverse column-transform identity mismatch");
}

void aliases_and_edge_patterns(
	test::engine_tests::EngineHarness &harness)
{
	for (const auto operation : operations)
		for (const auto registers : register_cases)
			for (unsigned pattern = 0; pattern < 16U; ++pattern)
				test::check_one(harness,
					{operation, registers},
					edge_state(pattern));
}

void every_byte_and_bit_basis(
	test::engine_tests::EngineHarness &harness)
{
	for (const auto operation : operations)
		for (unsigned byte = 0; byte < 16U; ++byte)
			for (unsigned bit = 0; bit < 8U; ++bit) {
				auto initial = test::state(
					static_cast<unsigned>(operation) * 100U +
					byte * 8U + bit);
				initial.vectors[0].bytes.fill(std::byte{});
				initial.vectors[1].bytes.fill(std::byte{});
				initial.vectors[1].bytes[byte] =
					static_cast<std::byte>(1U << bit);
				if (operation == test::Operation::encrypt_round ||
				    operation == test::Operation::decrypt_round)
					initial.vectors[0].bytes[
						(byte * 5U + 3U) & 15U] =
						static_cast<std::byte>(
							0x80U >> bit);
				test::check_one(harness,
					{operation, {0, 1}}, initial);
			}
}

void run_matrix(test::engine_tests::EngineHarnessOptions options,
	std::string_view summary)
{
	test::engine_tests::EngineHarness harness{options};
	aliases_and_edge_patterns(harness);
	every_byte_and_bit_basis(harness);
	constexpr std::size_t expected_cases =
		operations.size() * register_cases.size() * 16U +
		operations.size() * 16U * 8U;
	static_assert(expected_cases == 1'024U);
	test::require_engine_counts(harness, expected_cases);
	harness.print_summary(summary);
}

} // namespace

int main()
{
	oracle_known_answers();
	run_matrix({}, "arm64-simd-block-transform-behavior");
	run_matrix(
		{.force_software_aes = true},
		"arm64-simd-block-transform-portable");
	return 0;
}
