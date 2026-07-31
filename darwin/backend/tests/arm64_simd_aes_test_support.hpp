// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace arm64_simd_aes_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

enum class Operation : unsigned {
	encrypt_round = 4,
	decrypt_round = 5,
	mix_columns = 6,
	inverse_mix_columns = 7,
};

struct Registers final {
	unsigned destination;
	unsigned source;
};

struct Variant final {
	Operation operation;
	Registers registers;
};

[[noreturn]] inline void fail(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	std::abort();
}

inline void require(bool condition, const char *message)
{
	if (!condition)
		fail(message);
}

[[nodiscard]] constexpr std::string_view operation_name(
	Operation operation) noexcept
{
	switch (operation) {
	case Operation::encrypt_round: return "aese";
	case Operation::decrypt_round: return "aesd";
	case Operation::mix_columns: return "aesmc";
	case Operation::inverse_mix_columns: return "aesimc";
	}
	return "simd-block-transform";
}

/*
 * Direct transcription of the four encoding diagrams. The operation field
 * occupies bits 14:12; values 0..3 are reserved by this allocation.
 */
[[nodiscard]] constexpr std::uint32_t instruction(
	const Variant &variant) noexcept
{
	return 0x4e28'0800U |
		((static_cast<unsigned>(variant.operation) & 7U) << 12) |
		((variant.registers.source & 31U) << 5) |
		(variant.registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t reserved_instruction(
	unsigned operation, Registers registers) noexcept
{
	return 0x4e28'0800U | ((operation & 7U) << 12) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements() noexcept
{
	using uml::backend::ArmFeature;
	return engine_tests::NativeRequirements::features(
		{ArmFeature::advanced_simd, ArmFeature::aes});
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size();
		     ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed * 73U + reg * 41U + byte * 19U + 7U);
	for (unsigned reg = 0; reg < cpu.registers.size(); ++reg)
		cpu.registers[reg] =
			0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(seed + reg * 3U) *
			 0x0101'0101'0101'0101ULL);
	cpu.sp = 0xfeed'face'cafe'bee0ULL ^ seed;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'001fU;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	cpu.set_mte_configuration(true, 0xa55aU);
	cpu.set_mte_generator(true, 0x1357U, 9U,
		0x3141'5926'5358'9793ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = 0x89ab'cdefU;
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = true;
	cpu.pstate_tco = true;
	cpu.pstate_sm = true;
	cpu.pstate_za = true;
	cpu.pstate_daif_access = true;
	cpu.pstate_daif = 0xbU;
	cpu.exclusive_address = 0x1234'5000ULL;
	cpu.exclusive_key = 0x2468U;
	cpu.exclusive_value = 0x1122'3344'5566'7788ULL;
	cpu.exclusive_value2 = 0x8877'6655'4433'2211ULL;
	cpu.exclusive_version = 0x1020'3040'5060'7080ULL;
	cpu.exclusive_size = 8;
	return cpu;
}

/*
 * Independent mathematical oracle. It intentionally does not include or call
 * any production decoder, semantic helper, gadget, intrinsic, or JIT emitter.
 */
[[nodiscard]] constexpr std::uint8_t finite_multiply(
	std::uint8_t left, std::uint8_t right) noexcept
{
	std::uint8_t result = 0;
	for (unsigned bit = 0; bit < 8U; ++bit) {
		if ((right & 1U) != 0)
			result ^= left;
		const bool high = (left & 0x80U) != 0;
		left = static_cast<std::uint8_t>(left << 1U);
		if (high)
			left ^= 0x1bU;
		right = static_cast<std::uint8_t>(right >> 1U);
	}
	return result;
}

[[nodiscard]] constexpr std::uint8_t finite_inverse(
	std::uint8_t value) noexcept
{
	if (value == 0)
		return 0;
	std::uint8_t result = 1;
	std::uint8_t base = value;
	unsigned exponent = 254U;
	while (exponent != 0) {
		if ((exponent & 1U) != 0)
			result = finite_multiply(result, base);
		base = finite_multiply(base, base);
		exponent >>= 1U;
	}
	return result;
}

[[nodiscard]] constexpr std::uint8_t rotate_byte(
	std::uint8_t value, unsigned amount) noexcept
{
	return static_cast<std::uint8_t>(
		(value << amount) | (value >> (8U - amount)));
}

[[nodiscard]] consteval std::array<std::uint8_t, 256>
make_forward_substitution()
{
	std::array<std::uint8_t, 256> result{};
	for (unsigned value = 0; value < result.size(); ++value) {
		const auto inverse =
			finite_inverse(static_cast<std::uint8_t>(value));
		result[value] = static_cast<std::uint8_t>(
			inverse ^ rotate_byte(inverse, 1U) ^
			rotate_byte(inverse, 2U) ^ rotate_byte(inverse, 3U) ^
			rotate_byte(inverse, 4U) ^ 0x63U);
	}
	return result;
}

inline constexpr auto forward_substitution = make_forward_substitution();

[[nodiscard]] consteval std::array<std::uint8_t, 256>
make_inverse_substitution()
{
	std::array<std::uint8_t, 256> result{};
	for (unsigned value = 0; value < result.size(); ++value)
		result[forward_substitution[value]] =
			static_cast<std::uint8_t>(value);
	return result;
}

inline constexpr auto inverse_substitution = make_inverse_substitution();

[[nodiscard]] inline std::array<std::byte, 16> round_transform(
	const std::array<std::byte, 16> &destination,
	const std::array<std::byte, 16> &source, bool inverse)
{
	std::array<std::uint8_t, 16> combined{};
	std::array<std::byte, 16> result{};
	for (unsigned byte = 0; byte < combined.size(); ++byte)
		combined[byte] = std::to_integer<std::uint8_t>(
			destination[byte] ^ source[byte]);
	const auto &table =
		inverse ? inverse_substitution : forward_substitution;
	for (unsigned column = 0; column < 4U; ++column)
		for (unsigned row = 0; row < 4U; ++row) {
			const unsigned source_column = inverse ?
				(column + 4U - row) & 3U :
				(column + row) & 3U;
			result[column * 4U + row] = static_cast<std::byte>(
				table[combined[source_column * 4U + row]]);
		}
	return result;
}

[[nodiscard]] inline std::array<std::byte, 16> column_transform(
	const std::array<std::byte, 16> &source, bool inverse)
{
	std::array<std::byte, 16> result{};
	for (unsigned column = 0; column < 4U; ++column) {
		std::array<std::uint8_t, 4> input{};
		for (unsigned row = 0; row < input.size(); ++row)
			input[row] = std::to_integer<std::uint8_t>(
				source[column * 4U + row]);
		if (!inverse) {
			result[column * 4U] = static_cast<std::byte>(
				finite_multiply(input[0], 2U) ^
				finite_multiply(input[1], 3U) ^
				input[2] ^ input[3]);
			result[column * 4U + 1U] = static_cast<std::byte>(
				input[0] ^ finite_multiply(input[1], 2U) ^
				finite_multiply(input[2], 3U) ^ input[3]);
			result[column * 4U + 2U] = static_cast<std::byte>(
				input[0] ^ input[1] ^
				finite_multiply(input[2], 2U) ^
				finite_multiply(input[3], 3U));
			result[column * 4U + 3U] = static_cast<std::byte>(
				finite_multiply(input[0], 3U) ^ input[1] ^
				input[2] ^ finite_multiply(input[3], 2U));
		} else {
			result[column * 4U] = static_cast<std::byte>(
				finite_multiply(input[0], 14U) ^
				finite_multiply(input[1], 11U) ^
				finite_multiply(input[2], 13U) ^
				finite_multiply(input[3], 9U));
			result[column * 4U + 1U] = static_cast<std::byte>(
				finite_multiply(input[0], 9U) ^
				finite_multiply(input[1], 14U) ^
				finite_multiply(input[2], 11U) ^
				finite_multiply(input[3], 13U));
			result[column * 4U + 2U] = static_cast<std::byte>(
				finite_multiply(input[0], 13U) ^
				finite_multiply(input[1], 9U) ^
				finite_multiply(input[2], 14U) ^
				finite_multiply(input[3], 11U));
			result[column * 4U + 3U] = static_cast<std::byte>(
				finite_multiply(input[0], 11U) ^
				finite_multiply(input[1], 13U) ^
				finite_multiply(input[2], 9U) ^
				finite_multiply(input[3], 14U));
		}
	}
	return result;
}

inline void apply(arm64::CpuState &cpu, const Variant &variant)
{
	const auto destination =
		cpu.vectors[variant.registers.destination].bytes;
	const auto source = cpu.vectors[variant.registers.source].bytes;
	std::array<std::byte, 16> result{};
	switch (variant.operation) {
	case Operation::encrypt_round:
		result = round_transform(destination, source, false);
		break;
	case Operation::decrypt_round:
		result = round_transform(destination, source, true);
		break;
	case Operation::mix_columns:
		result = column_transform(source, false);
		break;
	case Operation::inverse_mix_columns:
		result = column_transform(source, true);
		break;
	}
	cpu.vectors[variant.registers.destination].bytes = result;
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, const Variant &variant)
{
	auto expected = initial;
	apply(expected, variant);
	return expected;
}

inline void check_one(engine_tests::EngineHarness &harness,
	const Variant &variant, const arm64::CpuState &initial)
{
	harness.check_one(operation_name(variant.operation),
		instruction(variant), initial, expected_state(initial, variant),
		native_requirements());
}

inline void require_engine_counts(
	const engine_tests::EngineHarness &harness,
	std::size_t valid_cases, std::size_t invalid_cases = 0U)
{
	const auto &counts = harness.counts();
	const auto total = valid_cases + invalid_cases;
	const auto expected_host_skips =
		engine_tests::host_supports(native_requirements()) ?
			0U : valid_cases;
	require(counts.interpreter == total,
		"block-transform interpreter case count is incomplete");
	require(counts.gadgets == total,
		"block-transform gadget case count is incomplete");
	require(counts.jit + counts.jit_skipped == total,
		"block-transform JIT case accounting is incomplete");
	require(counts.jit_skipped == expected_host_skips,
		"block-transform JIT skipped an unexpected case");
	require(counts.jit_skipped_host_feature == expected_host_skips,
		"block-transform host-feature skip count is wrong");
	require(counts.jit_unavailable == 0U,
		"block-transform native engine is unavailable");
}

} // namespace arm64_simd_aes_test
