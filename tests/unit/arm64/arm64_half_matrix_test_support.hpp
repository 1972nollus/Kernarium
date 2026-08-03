// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_half_dot_test_support.hpp"
#include "cpu/arm64/detail/half_matrix.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_half_matrix_test {

namespace shared = arm64_half_dot_test;
namespace exact = arm64_bfloat_arithmetic_test;
namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array register_aliases{
	Registers{0U, 1U, 2U}, Registers{1U, 1U, 2U},
	Registers{2U, 1U, 2U}, Registers{1U, 2U, 2U},
	Registers{3U, 3U, 3U}, Registers{31U, 30U, 29U},
	Registers{30U, 31U, 30U},
};

struct Result final {
	std::uint32_t bits{};
	std::uint32_t flags{};
};

using ExpectedCounts = shared::ExpectedCounts;

[[nodiscard]] constexpr std::uint32_t instruction(bool widening,
	Registers registers = {0U, 1U, 2U}) noexcept
{
	const std::uint32_t base =
		widening ? 0x4e40'ec00U : 0x4ec0'ec00U;
	return base | ((registers.second & 31U) << 16U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(bool widening) noexcept
{
	using ::uml::cpu::ArmFeature;
	return engine_tests::NativeRequirements::features({
		widening ? ArmFeature::f16f32mm : ArmFeature::f16mm,
		ArmFeature::afp,
	});
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0U)
{
	auto cpu = shared::state(seed);
	for (unsigned reg = 0U; reg < cpu.vectors.size(); ++reg)
		for (unsigned lane = 0U; lane < 8U; ++lane)
			shared::write_half(cpu.vectors[reg].bytes, lane,
				shared::half(static_cast<float>(
					static_cast<int>(
						(seed + reg * 5U + lane) % 13U) - 6)));
	cpu.fpcr = 0U;
	cpu.fpsr = 0x0800'0000U;
	return cpu;
}

inline void clear_vectors(arm64::CpuState &cpu)
{
	for (auto &vector : cpu.vectors)
		vector.bytes.fill(std::byte{});
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

[[nodiscard]] inline std::uint16_t read_half(
	const arm64::CpuState &cpu, unsigned reg, unsigned lane)
{
	return shared::read_half(cpu.vectors[reg].bytes, lane);
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

[[nodiscard]] constexpr bool half_subnormal(std::uint16_t bits) noexcept
{
	return (bits & 0x7c00U) == 0U && (bits & 0x03ffU) != 0U;
}

[[nodiscard]] constexpr std::uint16_t default_half_nan(
	std::uint32_t fpcr) noexcept
{
	return (fpcr & shared::fpcr_ah) != 0U ? 0xfe00U : 0x7e00U;
}

[[nodiscard]] constexpr std::uint16_t process_half_nan(
	std::uint16_t bits, std::uint32_t fpcr) noexcept
{
	return (fpcr & shared::fpcr_dn) != 0U ?
		default_half_nan(fpcr) :
		static_cast<std::uint16_t>(bits | 0x0200U);
}

[[nodiscard]] inline Result round_half(const exact::ExactValue &value,
	std::uint32_t fpcr)
{
	if (value.magnitude.zero())
		return {
			static_cast<std::uint32_t>(value.negative) << 15U, 0U,
		};
	const unsigned source_bits = value.magnitude.bit_length();
	int top_exponent =
		value.exponent + static_cast<int>(source_bits) - 1;
	const bool flush = (fpcr & shared::fpcr_fz16) != 0U;
	const int target_exponent =
		top_exponent >= -14 ? top_exponent - 10 : -24;
	const int signed_shift = target_exponent - value.exponent;
	exact::BigUnsigned retained = value.magnitude;
	unsigned shift{};
	if (signed_shift > 0) {
		shift = static_cast<unsigned>(signed_shift);
		retained = retained.shifted_right(shift);
	} else if (signed_shift < 0) {
		retained = retained.shifted_left(
			static_cast<unsigned>(-signed_shift));
	}
	const bool inexact =
		shift != 0U && value.magnitude.any_low_bits(shift);
	const unsigned rounding = (fpcr >> 22U) & 3U;
	if (inexact) {
		bool increment{};
		if (rounding == 0U) {
			const bool halfway =
				value.magnitude.bit(shift - 1U);
			const bool below = value.magnitude.any_low_bits(
				shift - 1U);
			increment = halfway && (below || retained.odd());
		} else if (rounding == 1U) {
			increment = !value.negative;
		} else if (rounding == 2U) {
			increment = value.negative;
		}
		if (increment)
			retained.increment();
	}
	if (top_exponent >= -14 &&
	    retained.compare(
		    exact::BigUnsigned{1U}.shifted_left(11U)) == 0) {
		retained = retained.shifted_right(1U);
		++top_exponent;
	}
	if (top_exponent >= 16) {
		const bool infinity = rounding == 0U ||
			(rounding == 1U && !value.negative) ||
			(rounding == 2U && value.negative);
		return {
			(static_cast<std::uint16_t>(value.negative) << 15U) |
				(infinity ? 0x7c00U : 0x7bffU),
			exact::fpsr_overflow | exact::fpsr_inexact,
		};
	}

	unsigned exponent_field{};
	std::uint16_t fraction{};
	if (top_exponent >= -14) {
		exponent_field =
			static_cast<unsigned>(top_exponent + 15);
		fraction = static_cast<std::uint16_t>(
			retained.low64()) & 0x03ffU;
	} else if (retained.compare(
			   exact::BigUnsigned{1U}.shifted_left(10U)) >= 0) {
		exponent_field = 1U;
	} else {
		fraction =
			static_cast<std::uint16_t>(retained.low64());
	}
	const bool subnormal =
		exponent_field == 0U && fraction != 0U;
	if (flush && subnormal) {
		const std::uint32_t flags =
			(fpcr & shared::fpcr_ah) != 0U ?
				exact::fpsr_underflow |
					exact::fpsr_inexact :
				exact::fpsr_underflow;
		return {
			static_cast<std::uint32_t>(value.negative) << 15U,
			flags,
		};
	}
	std::uint32_t flags{};
	if (inexact) {
		flags |= exact::fpsr_inexact;
		if (top_exponent < -14)
			flags |= exact::fpsr_underflow;
	}
	return {
		(static_cast<std::uint32_t>(value.negative) << 15U) |
			(exponent_field << 10U) |
			fraction,
		flags,
	};
}

[[nodiscard]] inline Result half_binary(std::uint16_t left_bits,
	std::uint16_t right_bits, bool multiply, std::uint32_t fpcr)
{
	const auto left = shared::unpack_half(left_bits, fpcr);
	const auto right = shared::unpack_half(right_bits, fpcr);
	const bool left_nan = left.type == exact::FloatingClass::nan;
	const bool right_nan = right.type == exact::FloatingClass::nan;
	if (left_nan || right_nan) {
		const bool any_signaling = left.signaling || right.signaling;
		if ((fpcr & shared::fpcr_ah) != 0U) {
			const auto selected = left_nan ? left_bits : right_bits;
			return {process_half_nan(selected, fpcr),
				any_signaling ? exact::fpsr_invalid : 0U};
		}
		for (const bool signaling : {true, false}) {
			if (left_nan && left.signaling == signaling)
				return {process_half_nan(left_bits, fpcr),
					signaling ? exact::fpsr_invalid : 0U};
			if (right_nan && right.signaling == signaling)
				return {process_half_nan(right_bits, fpcr),
					signaling ? exact::fpsr_invalid : 0U};
		}
	}
	const bool left_infinity =
		left.type == exact::FloatingClass::infinity;
	const bool right_infinity =
		right.type == exact::FloatingClass::infinity;
	const bool left_zero =
		left.type == exact::FloatingClass::finite &&
		left.magnitude.zero();
	const bool right_zero =
		right.type == exact::FloatingClass::finite &&
		right.magnitude.zero();
	const bool invalid = multiply ?
		((left_infinity && right_zero) ||
		 (right_infinity && left_zero)) :
		(left_infinity && right_infinity &&
		 left.negative != right.negative);
	if (invalid)
		return {default_half_nan(fpcr), exact::fpsr_invalid};
	if (left_infinity || right_infinity) {
		const bool negative = multiply ?
			left.negative != right.negative :
			(left_infinity ? left.negative : right.negative);
		return {
			(static_cast<std::uint16_t>(negative) << 15U) |
				0x7c00U,
			0U,
		};
	}
	const auto value = multiply ?
		exact::multiply_exact(left, right) :
		exact::add_exact(exact::exact(left), exact::exact(right),
			((fpcr >> 22U) & 3U) == 2U);
	return round_half(value, fpcr);
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, bool widening, Registers registers)
{
	const auto addends =
		initial.vectors[registers.destination].bytes;
	const auto first = initial.vectors[registers.first].bytes;
	const auto second = initial.vectors[registers.second].bytes;
	auto expected = initial;
	std::array<std::byte, 16> result{};
	std::uint32_t flags{};
	if (!widening) {
		for (unsigned segment = 0U; segment < 2U; ++segment) {
			for (unsigned row = 0U; row < 2U; ++row) {
				for (unsigned column = 0U; column < 2U;
				     ++column) {
					const unsigned lane =
						segment * 4U + row * 2U + column;
					const unsigned left_base =
						segment * 4U + row * 2U;
					const unsigned right_base =
						segment * 4U + column * 2U;
					const auto first_product = half_binary(
						shared::read_half(first, left_base),
						shared::read_half(second, right_base),
						true, initial.fpcr);
					const auto second_product = half_binary(
						shared::read_half(first,
							left_base + 1U),
						shared::read_half(second,
							right_base + 1U),
						true, initial.fpcr);
					const auto product_sum = half_binary(
						static_cast<std::uint16_t>(
							first_product.bits),
						static_cast<std::uint16_t>(
							second_product.bits),
						false, initial.fpcr);
					const auto sum = half_binary(
						shared::read_half(addends, lane),
						static_cast<std::uint16_t>(
							product_sum.bits),
						false, initial.fpcr);
					shared::write_half(result, lane,
						static_cast<std::uint16_t>(
							sum.bits));
					flags |= first_product.flags |
						second_product.flags |
						product_sum.flags | sum.flags;
				}
			}
		}
	} else {
		for (unsigned row = 0U; row < 2U; ++row) {
			for (unsigned column = 0U; column < 2U; ++column) {
				const unsigned lane = row * 2U + column;
				std::array<Result, 2> dots{};
				for (unsigned pair = 0U; pair < 2U; ++pair) {
					const unsigned left_base =
						row * 4U + pair * 2U;
					const unsigned right_base =
						column * 4U + pair * 2U;
					const auto dot = shared::dot_product(
						shared::read_half(first, left_base),
						shared::read_half(first,
							left_base + 1U),
						shared::read_half(second, right_base),
						shared::read_half(second,
							right_base + 1U),
						initial.fpcr);
					dots[pair] = {dot.bits, dot.flags};
				}
				const auto product_sum = shared::add_single(
					static_cast<std::uint32_t>(dots[0].bits),
					static_cast<std::uint32_t>(dots[1].bits),
					initial.fpcr);
				const auto sum = shared::add_single(
					exact::read_single(addends, lane),
					product_sum.bits, initial.fpcr);
				exact::write_single(result, lane, sum.bits);
				flags |= dots[0].flags | dots[1].flags |
					product_sum.flags | sum.flags;
			}
		}
	}
	expected.vectors[registers.destination].bytes = result;
	expected.fpsr |= flags;
	return expected;
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, bool widening, Registers registers,
	const arm64::CpuState &initial, ExpectedCounts &counts)
{
	const auto native = native_requirements(widening);
	harness.check_one(name, instruction(widening, registers), initial,
		expected_state(initial, widening, registers), native);
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

} // namespace arm64_half_matrix_test
