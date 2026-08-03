// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_half_dot_test_support.hpp"
#include "cpu/arm64/detail/half_fused_long.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_half_fused_long_test {

namespace shared = arm64_half_dot_test;
namespace exact = arm64_bfloat_arithmetic_test;
namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct Form final {
	bool by_element;
	bool subtract;
	bool upper;
	bool wide;
	unsigned index;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array vector_forms{
	Form{false, false, false, false, 0U},
	Form{false, false, false, true, 0U},
	Form{false, false, true, false, 0U},
	Form{false, false, true, true, 0U},
	Form{false, true, false, false, 0U},
	Form{false, true, false, true, 0U},
	Form{false, true, true, false, 0U},
	Form{false, true, true, true, 0U},
};

inline constexpr std::array register_aliases{
	Registers{0U, 1U, 2U}, Registers{1U, 1U, 2U},
	Registers{2U, 1U, 2U}, Registers{1U, 2U, 2U},
	Registers{3U, 3U, 3U}, Registers{31U, 30U, 29U},
	Registers{30U, 31U, 30U},
};
inline constexpr std::array indexed_register_aliases{
	Registers{0U, 1U, 2U}, Registers{1U, 1U, 2U},
	Registers{2U, 1U, 2U}, Registers{1U, 2U, 2U},
	Registers{3U, 3U, 3U}, Registers{31U, 30U, 15U},
	Registers{15U, 31U, 15U},
};

struct Result final {
	std::uint32_t bits{};
	std::uint32_t flags{};
};

using ExpectedCounts = shared::ExpectedCounts;

[[nodiscard]] constexpr Form indexed_form(bool subtract, bool upper,
	bool wide, unsigned index) noexcept
{
	return {true, subtract, upper, wide, index};
}

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	Registers registers = {0U, 1U, 2U}) noexcept
{
	std::uint32_t encoding;
	if (form.by_element) {
		encoding = form.upper ? 0x2f80'8000U : 0x0f80'0000U;
		if (form.subtract)
			encoding |= 0x0000'4000U;
		encoding |= ((form.index & 3U) << 20U) |
			((form.index & 4U) << 9U) |
			((registers.second & 15U) << 16U);
	} else {
		encoding = form.upper ? 0x2e20'cc00U : 0x0e20'ec00U;
		if (form.subtract)
			encoding |= 0x0080'0000U;
		encoding |= (registers.second & 31U) << 16U;
	}
	return encoding |
		(static_cast<std::uint32_t>(form.wide) << 30U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements() noexcept
{
	using ::uml::cpu::ArmFeature;
	return engine_tests::NativeRequirements::features({
		ArmFeature::fhm,
		ArmFeature::afp,
	});
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0U)
{
	auto cpu = shared::state(seed);
	for (unsigned reg = 0U; reg < cpu.vectors.size(); ++reg) {
		for (unsigned lane = 0U; lane < 4U; ++lane) {
			const float addend = static_cast<float>(
				static_cast<int>((seed + reg * 3U + lane) % 17U) - 8);
			exact::write_single(cpu.vectors[reg].bytes, lane,
				std::bit_cast<std::uint32_t>(addend));
		}
	}
	cpu.fpcr = 0U;
	cpu.fpsr = 0x0800'0000U;
	return cpu;
}

inline void write_half(arm64::CpuState &cpu, unsigned reg,
	unsigned lane, std::uint16_t bits)
{
	shared::write_half(cpu.vectors[reg].bytes, lane, bits);
}

inline void write_single(arm64::CpuState &cpu, unsigned reg,
	unsigned lane, std::uint32_t bits)
{
	exact::write_single(cpu.vectors[reg].bytes, lane, bits);
}

[[nodiscard]] inline std::uint32_t read_single(
	const arm64::CpuState &cpu, unsigned reg, unsigned lane)
{
	return exact::read_single(cpu.vectors[reg].bytes, lane);
}

[[nodiscard]] constexpr std::uint16_t half(float value) noexcept
{
	return shared::half(value);
}

[[nodiscard]] constexpr std::uint32_t converted_half_nan(
	std::uint16_t bits, std::uint32_t fpcr) noexcept
{
	return shared::convert_half_nan(bits, fpcr);
}

[[nodiscard]] constexpr Result process_single_nan(std::uint32_t bits,
	bool signaling, std::uint32_t fpcr) noexcept
{
	const auto result =
		shared::process_single_nan(bits, signaling, fpcr);
	return {result.bits, result.flags};
}

[[nodiscard]] inline Result fused(std::uint32_t addend_bits,
	std::uint16_t left_bits, std::uint16_t right_bits, bool subtract,
	std::uint32_t fpcr)
{
	const bool alternative = (fpcr & shared::fpcr_ah) != 0U;
	if (subtract && !(alternative &&
	    shared::unpack_half(left_bits, fpcr).type ==
		    exact::FloatingClass::nan))
		left_bits ^= 0x8000U;

	const auto left = shared::unpack_half(left_bits, fpcr);
	const auto right = shared::unpack_half(right_bits, fpcr);
	bool addend_flushed{};
	const bool fiz = (fpcr & shared::fpcr_fiz) != 0U;
	const bool standard_fz =
		!alternative && (fpcr & shared::fpcr_fz) != 0U;
	const auto addend = exact::unpack_single(addend_bits,
		fiz || standard_fz, addend_flushed);
	std::uint32_t flags = standard_fz && addend_flushed ?
		exact::fpsr_input_denormal : 0U;
	if (alternative && !fiz &&
	    shared::single_subnormal(addend_bits))
		flags |= exact::fpsr_input_denormal;

	const bool addend_nan = addend.type == exact::FloatingClass::nan;
	const bool left_nan = left.type == exact::FloatingClass::nan;
	const bool right_nan = right.type == exact::FloatingClass::nan;
	const bool any_signaling = addend.signaling || left.signaling ||
		right.signaling;
	if (addend_nan || left_nan || right_nan) {
		if (alternative) {
			if (left_nan && (addend_nan || right_nan))
				return {converted_half_nan(left_bits, fpcr),
					flags | (any_signaling ?
						exact::fpsr_invalid : 0U)};
			if (right_nan && addend_nan)
				return {converted_half_nan(right_bits, fpcr),
					flags | (any_signaling ?
						exact::fpsr_invalid : 0U)};
		}
		for (const bool signaling : {true, false}) {
			if (addend_nan && addend.signaling == signaling) {
				const auto result = process_single_nan(
					addend_bits, signaling, fpcr);
				return {result.bits, flags | result.flags};
			}
			if (left_nan && left.signaling == signaling)
				return {converted_half_nan(left_bits, fpcr),
					flags | (signaling ?
						exact::fpsr_invalid : 0U)};
			if (right_nan && right.signaling == signaling)
				return {converted_half_nan(right_bits, fpcr),
					flags | (signaling ?
						exact::fpsr_invalid : 0U)};
		}
	}

	const auto finite_zero = [](const exact::BinaryValue &value) {
		return value.type == exact::FloatingClass::finite &&
			value.magnitude.zero();
	};
	const bool left_infinity =
		left.type == exact::FloatingClass::infinity;
	const bool right_infinity =
		right.type == exact::FloatingClass::infinity;
	const bool addend_infinity =
		addend.type == exact::FloatingClass::infinity;
	const bool product_infinity = left_infinity || right_infinity;
	const bool product_zero = finite_zero(left) || finite_zero(right);
	const bool product_negative = left.negative != right.negative;
	if ((left_infinity && finite_zero(right)) ||
	    (right_infinity && finite_zero(left)) ||
	    (addend_infinity && product_infinity &&
	     addend.negative != product_negative))
		return {shared::default_nan(fpcr),
			flags | exact::fpsr_invalid};
	if (addend_infinity)
		return {
			(static_cast<std::uint32_t>(addend.negative) << 31U) |
				0x7f80'0000U,
			flags,
		};
	if (product_infinity)
		return {
			(static_cast<std::uint32_t>(product_negative) << 31U) |
				0x7f80'0000U,
			flags,
		};

	const auto product = product_zero ?
		exact::ExactValue{product_negative, 0U, 0} :
		exact::multiply_exact(left, right);
	const auto sum = exact::add_exact(exact::exact(addend), product,
		((fpcr >> 22U) & 3U) == 2U);
	const auto rounded = shared::round_single(sum, fpcr);
	return {rounded.bits, flags | rounded.flags};
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, Form form, Registers registers)
{
	const auto addends =
		initial.vectors[registers.destination].bytes;
	const auto first = initial.vectors[registers.first].bytes;
	const auto second = initial.vectors[registers.second].bytes;
	auto expected = initial;
	std::array<std::byte, 16> result{};
	std::uint32_t flags{};
	const unsigned lanes = form.wide ? 4U : 2U;
	const unsigned first_lane = form.upper ? lanes : 0U;
	for (unsigned lane = 0U; lane < lanes; ++lane) {
		const unsigned second_lane =
			form.by_element ? form.index : first_lane + lane;
		const auto lane_result = fused(
			exact::read_single(addends, lane),
			shared::read_half(first, first_lane + lane),
			shared::read_half(second, second_lane),
			form.subtract, initial.fpcr);
		exact::write_single(result, lane, lane_result.bits);
		flags |= lane_result.flags;
	}
	expected.vectors[registers.destination].bytes = result;
	expected.fpsr |= flags;
	return expected;
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, Form form, Registers registers,
	const arm64::CpuState &initial, ExpectedCounts &counts)
{
	const auto native = native_requirements();
	harness.check_one(name, instruction(form, registers), initial,
		expected_state(initial, form, registers), native);
	counts.add(native);
}

inline void require_counts(const engine_tests::EngineHarness &harness,
	const ExpectedCounts &counts)
{
	shared::require_counts(harness, counts);
}

inline void require(bool condition, const char *message)
{
	shared::require(condition, message);
}

} // namespace arm64_half_fused_long_test
