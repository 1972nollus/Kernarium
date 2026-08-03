// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace uml::emulator::tests::base_multiply {

namespace arm64 = uml::cpu::arm64;

enum class Form {
	regular,
	signed_long,
	signed_high,
	unsigned_long,
	unsigned_high,
};

struct FormCase final {
	bool wide;
	unsigned operation;
	bool subtract;
	std::string_view name;
};

inline constexpr std::array legal_forms{
	FormCase{false, 0, false, "MADD W"},
	FormCase{false, 0, true, "MSUB W"},
	FormCase{true, 0, false, "MADD X"},
	FormCase{true, 0, true, "MSUB X"},
	FormCase{true, 1, false, "SMADDL"},
	FormCase{true, 1, true, "SMSUBL"},
	FormCase{true, 5, false, "UMADDL"},
	FormCase{true, 5, true, "UMSUBL"},
	FormCase{true, 2, false, "SMULH"},
	FormCase{true, 6, false, "UMULH"},
};

[[nodiscard]] constexpr bool is_high(const FormCase &form) noexcept
{
	return form.operation == 2U || form.operation == 6U;
}

[[nodiscard]] constexpr std::uint32_t encode(
	bool wide, unsigned op54, unsigned operation, bool subtract,
	unsigned right, unsigned addend, unsigned left,
	unsigned destination) noexcept
{
	return 0x1b00'0000U | (wide ? 0x8000'0000U : 0U) |
		((op54 & 3U) << 29) | ((operation & 7U) << 21) |
		((right & 31U) << 16) | (subtract ? 0x0000'8000U : 0U) |
		((addend & 31U) << 10) | ((left & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode(
	const FormCase &form, unsigned left, unsigned right,
	unsigned addend, unsigned destination) noexcept
{
	return encode(form.wide, 0, form.operation, form.subtract,
		right, addend, left, destination);
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index) {
		const auto ordinal = static_cast<std::uint64_t>(index + 1U);
		cpu.registers[index] =
			(seed ^ (ordinal * 0x9e37'79b9'7f4a'7c15ULL)) +
			(ordinal * 0x0102'0304'0506'0708ULL);
	}
	cpu.sp = seed ^ 0xfedc'ba98'7654'3210ULL;
	cpu.nzcv = static_cast<std::uint32_t>((seed >> 7) & 0xfU) << 28;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed + reg * 29U + byte * 17U);
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.tls = seed ^ 0x0123'4567'89ab'cdefULL;
	cpu.set_mte_configuration(true, 0x5a5aU);
	cpu.set_mte_generator(true, 0x1357U, 9U,
		seed ^ 0x6a09'e667'f3bc'c909ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = static_cast<std::uint32_t>(seed);
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = true;
	cpu.pstate_tco = true;
	cpu.pstate_sm = true;
	cpu.pstate_za = true;
	cpu.pstate_daif_access = true;
	cpu.pstate_daif = 0xbU;
	cpu.exclusive_address = seed ^ 0x8877'6655'4433'2210ULL;
	cpu.exclusive_key = static_cast<std::uintptr_t>(
		seed ^ 0x1020'3040'5060'7080ULL);
	cpu.exclusive_value = seed ^ 0x1111'2222'3333'4444ULL;
	cpu.exclusive_value2 = seed ^ 0x5555'6666'7777'8888ULL;
	cpu.exclusive_version = seed ^ 0x9999'aaaa'bbbb'ccccULL;
	cpu.exclusive_size = 8;
	cpu.fpmr = seed ^ 0x0f1e'2d3c'4b5a'6978ULL;
	return cpu;
}

[[nodiscard]] constexpr bool is_other_feature_partition(
	std::uint32_t instruction) noexcept
{
	const bool wide = (instruction & 0x8000'0000U) != 0U;
	const unsigned op54 = (instruction >> 29) & 3U;
	const unsigned operation = (instruction >> 21) & 7U;
	return wide && op54 == 0U && operation == 3U;
}

/*
 * Independent transcription of the Data-processing (3 source) allocation.
 * The checked-pointer operation==3 partition belongs to another feature and
 * is deliberately not classified as either this family or reserved here.
 */
[[nodiscard]] constexpr bool is_valid(std::uint32_t instruction) noexcept
{
	const bool wide = (instruction & 0x8000'0000U) != 0U;
	const unsigned op54 = (instruction >> 29) & 3U;
	const unsigned operation = (instruction >> 21) & 7U;
	const bool subtract = (instruction & 0x0000'8000U) != 0U;
	const unsigned addend = (instruction >> 10) & 31U;
	if (op54 != 0U)
		return false;
	if (!wide)
		return operation == 0U;
	if (operation == 0U || operation == 1U || operation == 5U)
		return true;
	if (operation == 2U || operation == 6U)
		return !subtract && addend == 31U;
	return false;
}

[[nodiscard]] constexpr std::optional<Form>
form_for(std::uint32_t instruction) noexcept
{
	if (!is_valid(instruction))
		return std::nullopt;
	const unsigned operation = (instruction >> 21) & 7U;
	if (operation == 0U)
		return Form::regular;
	if (operation == 1U)
		return Form::signed_long;
	if (operation == 2U)
		return Form::signed_high;
	if (operation == 5U)
		return Form::unsigned_long;
	return Form::unsigned_high;
}

[[nodiscard]] constexpr std::uint64_t register_value(
	const arm64::CpuState &cpu, unsigned index) noexcept
{
	return index == 31U ? 0U : cpu.registers[index];
}

[[nodiscard]] constexpr std::int64_t signed_word(
	std::uint64_t value) noexcept
{
	const auto bits = static_cast<std::uint32_t>(value);
	if ((bits & 0x8000'0000U) == 0U)
		return bits;
	const auto magnitude = static_cast<std::uint32_t>(~bits + 1U);
	return -static_cast<std::int64_t>(magnitude);
}

[[nodiscard]] constexpr __int128 signed_double_word(
	std::uint64_t value) noexcept
{
	constexpr auto sign = std::uint64_t{1} << 63;
	if ((value & sign) == 0U)
		return static_cast<__int128>(value);
	return static_cast<__int128>(value & (sign - 1U)) -
		(static_cast<__int128>(1U) << 63);
}

[[nodiscard]] constexpr std::uint64_t signed_high_product(
	std::uint64_t left, std::uint64_t right) noexcept
{
	const auto product = signed_double_word(left) *
		signed_double_word(right);
	return static_cast<std::uint64_t>(
		static_cast<unsigned __int128>(product) >> 64);
}

[[nodiscard]] constexpr std::uint64_t unsigned_high_product(
	std::uint64_t left, std::uint64_t right) noexcept
{
	return static_cast<std::uint64_t>(
		(static_cast<unsigned __int128>(left) * right) >> 64);
}

[[nodiscard]] inline arm64::CpuState expected_state(
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	const auto form = form_for(instruction);
	if (!form) {
		std::fprintf(stderr,
			"multiply oracle asked to execute reserved instruction "
			"%#010x\n", instruction);
		std::abort();
	}
	const unsigned destination = instruction & 31U;
	const unsigned left_index = (instruction >> 5) & 31U;
	const unsigned addend_index = (instruction >> 10) & 31U;
	const unsigned right_index = (instruction >> 16) & 31U;
	const bool subtract = (instruction & 0x0000'8000U) != 0U;
	const auto left = register_value(initial, left_index);
	const auto right = register_value(initial, right_index);
	const auto addend = register_value(initial, addend_index);

	std::uint64_t result;
	switch (*form) {
	case Form::regular: {
		const bool wide = (instruction & 0x8000'0000U) != 0U;
		const auto mask = wide ? ~std::uint64_t{0} :
			std::uint64_t{0xffff'ffffU};
		const auto product = (left & mask) * (right & mask);
		result = (subtract ? (addend & mask) - product :
			(addend & mask) + product) & mask;
		break;
	}
	case Form::signed_long: {
		const auto product = signed_word(left) * signed_word(right);
		const auto product_bits = static_cast<std::uint64_t>(product);
		result = subtract ? addend - product_bits :
			addend + product_bits;
		break;
	}
	case Form::unsigned_long: {
		const auto product =
			static_cast<std::uint64_t>(
				static_cast<std::uint32_t>(left)) *
			static_cast<std::uint32_t>(right);
		result = subtract ? addend - product : addend + product;
		break;
	}
	case Form::signed_high:
		result = signed_high_product(left, right);
		break;
	case Form::unsigned_high:
		result = unsigned_high_product(left, right);
		break;
	}

	auto expected = initial;
	if (destination != 31U)
		expected.registers[destination] = result;
	return expected;
}

inline void check(EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	if (!is_valid(instruction)) {
		std::fprintf(stderr,
			"valid multiply case has reserved instruction %#010x\n",
			instruction);
		std::abort();
	}
	harness.check_one(name, instruction, initial,
		expected_state(instruction, initial),
		NativeRequirements::baseline());
}

inline void check_illegal(EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	if (is_valid(instruction) || is_other_feature_partition(instruction)) {
		std::fprintf(stderr,
			"reserved multiply case is allocated instruction %#010x\n",
			instruction);
		std::abort();
	}
	harness.check_illegal(name, instruction, initial,
		NativeRequirements::baseline());
}

void run_allocation_tests(EngineHarness &harness);
void run_behavior_tests(EngineHarness &harness);
void run_reserved_tests(EngineHarness &harness);

} // namespace uml::emulator::tests::base_multiply
