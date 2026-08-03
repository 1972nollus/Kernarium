// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string_view>

namespace arm64_simd_dot_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Mode {
	signed_signed,
	unsigned_unsigned,
	signed_unsigned,
	unsigned_signed,
};

enum class Form {
	vector,
	indexed,
};

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

inline constexpr std::array vector_modes{
	Mode::signed_signed,
	Mode::unsigned_unsigned,
	Mode::unsigned_signed,
};

inline constexpr std::array indexed_modes{
	Mode::signed_signed,
	Mode::unsigned_unsigned,
	Mode::signed_unsigned,
	Mode::unsigned_signed,
};

inline constexpr std::array all_forms{
	std::pair{Form::vector, Mode::signed_signed},
	std::pair{Form::vector, Mode::unsigned_unsigned},
	std::pair{Form::vector, Mode::unsigned_signed},
	std::pair{Form::indexed, Mode::signed_signed},
	std::pair{Form::indexed, Mode::unsigned_unsigned},
	std::pair{Form::indexed, Mode::signed_unsigned},
	std::pair{Form::indexed, Mode::unsigned_signed},
};

[[noreturn]] inline void fail(const char *message,
	const std::source_location location = std::source_location::current())
{
	std::fprintf(stderr, "%s at %s:%u\n", message,
		location.file_name(), location.line());
	std::abort();
}

inline void require(bool condition, const char *message,
	const std::source_location location = std::source_location::current())
{
	if (!condition)
		fail(message, location);
}

[[nodiscard]] constexpr bool left_signed(Mode mode) noexcept
{
	return mode == Mode::signed_signed || mode == Mode::signed_unsigned;
}

[[nodiscard]] constexpr bool right_signed(Mode mode) noexcept
{
	return mode == Mode::signed_signed || mode == Mode::unsigned_signed;
}

[[nodiscard]] constexpr bool extended(Mode mode) noexcept
{
	return mode == Mode::signed_unsigned || mode == Mode::unsigned_signed;
}

[[nodiscard]] constexpr std::uint32_t vector_instruction(Mode mode, bool q,
	unsigned size, Registers registers) noexcept
{
	/*
	 * The mixed vector allocation has only the unsigned-by-signed form.
	 * Its fixed size field is kept explicit so allocation tests can exercise
	 * all four values without borrowing an implementation decoder.
	 */
	const bool ordinary = !extended(mode);
	const bool unsigned_group = mode == Mode::unsigned_unsigned;
	return (ordinary ? 0x0e00'9400U : 0x0e00'9c00U) |
		(q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t indexed_instruction_with_size(
	Mode mode, bool q, unsigned size, Registers registers,
	unsigned index) noexcept
{
	const bool ordinary = !extended(mode);
	const bool unsigned_group = mode == Mode::unsigned_unsigned;
	std::uint32_t instruction = 0x0f00'0000U |
		(q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) |
		((ordinary ? 14U : 15U) << 12) |
		((registers.right & 15U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
	/*
	 * For every instruction in this family, M extends Rm and H:L selects
	 * one of the four groups. Other allocations sharing opcode 1111 give
	 * these bits different meanings and are intentionally not classified
	 * as this family.
	 */
	instruction |= ((registers.right >> 4) & 1U) << 20;
	instruction |= (index & 1U) << 21;
	instruction |= (index & 2U) << 10;
	return instruction;
}

[[nodiscard]] constexpr std::uint32_t indexed_instruction(
	Mode mode, bool q, Registers registers, unsigned index) noexcept
{
	return indexed_instruction_with_size(mode, q,
		mode == Mode::signed_unsigned ? 0U : 2U, registers, index);
}

[[nodiscard]] constexpr std::uint32_t instruction(Form form, Mode mode,
	bool q, Registers registers, unsigned index = 0U) noexcept
{
	return form == Form::vector ?
		vector_instruction(mode, q, 2U, registers) :
		indexed_instruction(mode, q, registers, index);
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(Mode mode)
{
	return engine_tests::NativeRequirements::features(
		extended(mode) ? ::uml::cpu::ArmFeature::i8mm :
			::uml::cpu::ArmFeature::dotprod);
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed * 67U + reg * 29U + byte * 17U + 3U);
	for (unsigned reg = 0; reg < cpu.registers.size(); ++reg)
		cpu.registers[reg] = 0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(reg + seed) *
				0x0101'0101'0101'0101ULL);
	cpu.sp = 0xfeed'face'cafe'bee0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0140'0000U;
	cpu.fpsr = 0x0000'009fU;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL ^ seed;
	cpu.tls = 0xfedc'ba98'7654'3210ULL ^ seed;
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
	cpu.exclusive_size = 8U;
	cpu.gcs_pointer = 0x6000'1000ULL;
	cpu.gcs_implemented = true;
	cpu.gcs_push_enabled = true;
	cpu.gcs_store_enabled = true;
	return cpu;
}

[[nodiscard]] inline std::uint32_t read_word(
	const std::array<std::byte, 16> &bytes, unsigned offset) noexcept
{
	std::uint32_t value = 0;
	for (unsigned byte = 0; byte < 4U; ++byte)
		value |= static_cast<std::uint32_t>(
			std::to_integer<unsigned>(bytes[offset + byte])) <<
			(byte * 8U);
	return value;
}

inline void write_word(std::array<std::byte, 16> &bytes, unsigned offset,
	std::uint32_t value) noexcept
{
	for (unsigned byte = 0; byte < 4U; ++byte)
		bytes[offset + byte] =
			static_cast<std::byte>(value >> (byte * 8U));
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, Mode mode, bool q, bool by_element,
	Registers registers, unsigned index)
{
	const auto old_destination =
		initial.vectors[registers.destination].bytes;
	const auto old_left = initial.vectors[registers.left].bytes;
	const auto old_right = initial.vectors[registers.right].bytes;
	const unsigned vector_bytes = q ? 16U : 8U;
	auto expected = initial;
	expected.vectors[registers.destination] = {};
	for (unsigned group = 0; group < vector_bytes / 4U; ++group) {
		std::int64_t sum = 0;
		for (unsigned byte = 0; byte < 4U; ++byte) {
			const auto left_raw = std::to_integer<std::uint8_t>(
				old_left[group * 4U + byte]);
			const auto right_raw = std::to_integer<std::uint8_t>(
				old_right[(by_element ? index : group) * 4U + byte]);
			const std::int64_t left =
				left_signed(mode) ?
					static_cast<std::int8_t>(left_raw) : left_raw;
			const std::int64_t right =
				right_signed(mode) ?
					static_cast<std::int8_t>(right_raw) : right_raw;
			sum += left * right;
		}
		const auto accumulator = read_word(old_destination, group * 4U);
		write_word(expected.vectors[registers.destination].bytes,
			group * 4U,
			accumulator + static_cast<std::uint32_t>(sum));
	}
	return expected;
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, Form form, Mode mode, bool q,
	Registers registers, unsigned index, const arm64::CpuState &initial)
{
	harness.check_one(name,
		instruction(form, mode, q, registers, index), initial,
		expected_state(initial, mode, q, form == Form::indexed,
			registers, index),
		native_requirements(mode));
}

} // namespace arm64_simd_dot_test
