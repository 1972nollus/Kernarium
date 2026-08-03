// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_dot_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_simd_matrix_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;
namespace dot = arm64_simd_dot_test;

using Registers = dot::Registers;
using dot::fail;
using dot::require;

enum class Mode {
	signed_signed,
	unsigned_unsigned,
	unsigned_signed,
};

inline constexpr std::array modes{
	Mode::signed_signed,
	Mode::unsigned_unsigned,
	Mode::unsigned_signed,
};

[[nodiscard]] constexpr std::string_view inventory_name(Mode mode) noexcept
{
	switch (mode) {
	case Mode::signed_signed: return "FPSIMD-0326 SMMLA vector";
	case Mode::unsigned_unsigned: return "FPSIMD-0433 UMMLA vector";
	case Mode::unsigned_signed: return "FPSIMD-0456 USMMLA vector";
	}
	return "SIMD matrix";
}

[[nodiscard]] constexpr std::uint32_t raw_instruction(bool q, bool u,
	unsigned size, bool alternate, Registers registers) noexcept
{
	return 0x0e00'a400U |
		(q ? 0x4000'0000U : 0U) |
		(u ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) |
		(alternate ? 0x0000'0800U : 0U) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr bool valid_encoding(bool q, bool u,
	unsigned size, bool alternate) noexcept
{
	return q && size == 2U && !(u && alternate);
}

[[nodiscard]] constexpr Mode mode_for(bool u, bool alternate) noexcept
{
	return u ? Mode::unsigned_unsigned :
		alternate ? Mode::unsigned_signed : Mode::signed_signed;
}

[[nodiscard]] constexpr std::uint32_t instruction(Mode mode,
	Registers registers) noexcept
{
	return raw_instruction(true, mode == Mode::unsigned_unsigned, 2U,
		mode == Mode::unsigned_signed, registers);
}

[[nodiscard]] constexpr bool left_unsigned(Mode mode) noexcept
{
	return mode != Mode::signed_signed;
}

[[nodiscard]] constexpr bool right_unsigned(Mode mode) noexcept
{
	return mode == Mode::unsigned_unsigned;
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::features(
		::uml::cpu::ArmFeature::i8mm);
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0)
{
	return dot::state(seed);
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, Mode mode, Registers registers)
{
	/*
	 * Snapshot all three operands before producing any result. This is
	 * required for every destructive alias, including Vd == Vn == Vm.
	 */
	const auto accumulator =
		initial.vectors[registers.destination].bytes;
	const auto left = initial.vectors[registers.left].bytes;
	const auto right = initial.vectors[registers.right].bytes;
	std::array<std::byte, 16> result{};
	for (unsigned row = 0; row < 2U; ++row)
		for (unsigned column = 0; column < 2U; ++column) {
			std::int64_t sum = 0;
			for (unsigned k = 0; k < 8U; ++k) {
				const auto left_raw = std::to_integer<std::uint8_t>(
					left[row * 8U + k]);
				const auto right_raw = std::to_integer<std::uint8_t>(
					right[column * 8U + k]);
				const std::int64_t left_value = left_unsigned(mode) ?
					left_raw : static_cast<std::int8_t>(left_raw);
				const std::int64_t right_value = right_unsigned(mode) ?
					right_raw : static_cast<std::int8_t>(right_raw);
				sum += left_value * right_value;
			}
			const unsigned lane = row * 2U + column;
			const auto addend =
				dot::read_word(accumulator, lane * 4U);
			dot::write_word(result, lane * 4U,
				addend + static_cast<std::uint32_t>(sum));
		}
	auto expected = initial;
	expected.vectors[registers.destination].bytes = result;
	return expected;
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, Mode mode, Registers registers,
	const arm64::CpuState &initial)
{
	harness.check_one(name, instruction(mode, registers), initial,
		expected_state(initial, mode, registers), native_requirements());
}

} // namespace arm64_simd_matrix_test
