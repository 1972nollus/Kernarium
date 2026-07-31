// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace arm64_simd_three_same_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

[[nodiscard]] constexpr std::uint32_t instruction(bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode, Registers registers)
{
	return (scalar ? 0x5e20'0400U : 0x0e20'0400U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) | ((opcode & 31U) << 11) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr bool valid(bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode)
{
	if (opcode >= 24U || opcode == 3U)
		return false;
	if (scalar) {
		if (opcode == 1U || opcode == 5U || opcode == 9U ||
		    opcode == 11U)
			return true;
		if (opcode == 6U || opcode == 7U || opcode == 8U ||
		    opcode == 10U || opcode == 16U || opcode == 17U)
			return size == 3U;
		return opcode == 22U && (size == 1U || size == 2U);
	}
	if (!q && size == 3U)
		return false;
	if (size == 3U)
		return opcode == 1U || opcode == 5U || opcode == 6U ||
			opcode == 7U || opcode == 8U || opcode == 9U ||
			opcode == 10U || opcode == 11U || opcode == 16U ||
			opcode == 17U || (opcode == 23U && !unsigned_group);
	if (opcode == 19U && unsigned_group)
		return size == 0U;
	if (opcode == 22U)
		return size == 1U || size == 2U;
	if (opcode == 23U)
		return !unsigned_group;
	return true;
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
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = seed & 1U;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	return cpu;
}

[[nodiscard]] constexpr std::uint64_t mask(unsigned bits)
{
	return bits == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << bits) - 1U;
}

[[nodiscard]] inline std::uint64_t read_lane(
	const std::array<std::byte, 16> &bytes, std::size_t offset,
	std::size_t size)
{
	std::uint64_t value = 0;
	for (std::size_t byte = 0; byte < size; ++byte)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(bytes[offset + byte])) <<
			(byte * 8U);
	return value;
}

inline void write_lane(std::array<std::byte, 16> &bytes,
	std::size_t offset, std::size_t size, std::uint64_t value)
{
	for (std::size_t byte = 0; byte < size; ++byte)
		bytes[offset + byte] =
			static_cast<std::byte>(value >> (byte * 8U));
}

inline void fill_lanes(arm64::VectorRegister &vector,
	std::uint64_t value, unsigned size)
{
	const std::size_t lane_size = std::size_t{1} << size;
	for (std::size_t offset = 0; offset < vector.bytes.size();
	     offset += lane_size)
		write_lane(vector.bytes, offset, lane_size, value);
}

[[nodiscard]] constexpr __int128 signed_value(
	std::uint64_t raw, unsigned bits)
{
	raw &= mask(bits);
	if ((raw & (std::uint64_t{1} << (bits - 1U))) == 0)
		return static_cast<__int128>(raw);
	return static_cast<__int128>(raw) -
		(static_cast<__int128>(1) << bits);
}

[[nodiscard]] constexpr std::uint64_t truncate(
	__int128 value, unsigned bits)
{
	return static_cast<std::uint64_t>(
		static_cast<unsigned __int128>(value)) & mask(bits);
}

[[nodiscard]] constexpr __int128 floor_shift_right(
	__int128 value, unsigned shift)
{
	if (shift == 0U)
		return value;
	const __int128 divisor = static_cast<__int128>(1) << shift;
	if (value >= 0)
		return value / divisor;
	return -((-value + divisor - 1) / divisor);
}

[[nodiscard]] constexpr __int128 shift_right_signed(
	__int128 value, unsigned shift, bool rounding)
{
	if (rounding && shift != 0U)
		value += static_cast<__int128>(1) << (shift - 1U);
	return floor_shift_right(value, shift);
}

[[nodiscard]] constexpr unsigned __int128 shift_right_unsigned(
	unsigned __int128 value, unsigned shift, bool rounding)
{
	if (rounding && shift != 0U)
		value += static_cast<unsigned __int128>(1) << (shift - 1U);
	return value >> shift;
}

[[nodiscard]] constexpr int saturated_shift(std::uint64_t raw,
	unsigned bits)
{
	const unsigned low_byte = static_cast<unsigned>(raw & 0xffU);
	int shift = low_byte < 0x80U ? static_cast<int>(low_byte) :
		static_cast<int>(low_byte) - 0x100;
	const int limit = static_cast<int>(bits) + 1;
	if (shift > limit)
		shift = limit;
	if (shift < -limit)
		shift = -limit;
	return shift;
}

[[nodiscard]] inline std::uint64_t saturate_signed(
	__int128 value, unsigned bits, bool &saturated)
{
	const __int128 minimum =
		-(static_cast<__int128>(1) << (bits - 1U));
	const __int128 maximum =
		(static_cast<__int128>(1) << (bits - 1U)) - 1;
	if (value < minimum) {
		value = minimum;
		saturated = true;
	} else if (value > maximum) {
		value = maximum;
		saturated = true;
	}
	return truncate(value, bits);
}

[[nodiscard]] inline std::uint64_t saturate_unsigned(
	__int128 value, unsigned bits, bool &saturated)
{
	const auto maximum =
		(static_cast<unsigned __int128>(1) << bits) - 1U;
	if (value < 0) {
		saturated = true;
		return 0;
	}
	const auto unsigned_value = static_cast<unsigned __int128>(value);
	if (unsigned_value > maximum) {
		saturated = true;
		return static_cast<std::uint64_t>(maximum);
	}
	return static_cast<std::uint64_t>(unsigned_value);
}

[[nodiscard]] constexpr std::uint64_t polynomial_multiply_byte(
	std::uint64_t left, std::uint64_t right)
{
	std::uint64_t product = 0;
	for (unsigned bit = 0; bit < 8U; ++bit)
		if ((right & (std::uint64_t{1} << bit)) != 0)
			product ^= left << bit;
	return product & 0xffU;
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode, Registers registers)
{
	const auto left = initial.vectors[registers.left].bytes;
	const auto right = initial.vectors[registers.right].bytes;
	const auto old_destination =
		initial.vectors[registers.destination].bytes;
	std::array<std::byte, 16> result{};
	const std::size_t lane_size = std::size_t{1} << size;
	const unsigned lane_bits = static_cast<unsigned>(lane_size * 8U);
	const std::size_t vector_size = scalar ? lane_size : q ? 16U : 8U;
	const std::size_t lanes = vector_size / lane_size;
	bool saturated = false;

	const auto lane = [&](const auto &bytes, std::size_t index) {
		return read_lane(bytes, index * lane_size, lane_size);
	};
	const auto write = [&](std::size_t index, std::uint64_t value) {
		write_lane(result, index * lane_size, lane_size,
			value & mask(lane_bits));
	};

	if (!scalar && (opcode == 20U || opcode == 21U || opcode == 23U)) {
		for (std::size_t output = 0; output < lanes; ++output) {
			const auto &source = output < lanes / 2U ? left : right;
			const auto pair = (output % (lanes / 2U)) * 2U;
			const auto first = lane(source, pair);
			const auto second = lane(source, pair + 1U);
			std::uint64_t value = 0;
			if (opcode == 23U) {
				value = first + second;
			} else if (unsigned_group) {
				value = opcode == 20U ? std::max(first, second) :
					std::min(first, second);
			} else {
				const auto first_signed =
					signed_value(first, lane_bits);
				const auto second_signed =
					signed_value(second, lane_bits);
				value = truncate(opcode == 20U ?
					std::max(first_signed, second_signed) :
					std::min(first_signed, second_signed), lane_bits);
			}
			write(output, value);
		}
	} else {
		for (std::size_t index = 0; index < lanes; ++index) {
			const auto first = lane(left, index);
			const auto second = lane(right, index);
			const auto old = lane(old_destination, index);
			const auto first_signed = signed_value(first, lane_bits);
			const auto second_signed = signed_value(second, lane_bits);
			std::uint64_t value = 0;

			switch (opcode) {
			case 0: {
				const __int128 sum = unsigned_group ?
					static_cast<__int128>(first) + second :
					first_signed + second_signed;
				value = truncate(floor_shift_right(sum, 1U), lane_bits);
				break;
			}
			case 1:
				value = unsigned_group ?
					saturate_unsigned(
						static_cast<__int128>(first) + second,
						lane_bits, saturated) :
					saturate_signed(first_signed + second_signed,
						lane_bits, saturated);
				break;
			case 2: {
				const __int128 sum = unsigned_group ?
					static_cast<__int128>(first) + second :
					first_signed + second_signed;
				value = truncate(
					shift_right_signed(sum, 1U, true), lane_bits);
				break;
			}
			case 4: {
				const __int128 difference = unsigned_group ?
					static_cast<__int128>(first) - second :
					first_signed - second_signed;
				value = truncate(
					floor_shift_right(difference, 1U), lane_bits);
				break;
			}
			case 5:
				value = unsigned_group ?
					saturate_unsigned(
						static_cast<__int128>(first) - second,
						lane_bits, saturated) :
					saturate_signed(first_signed - second_signed,
						lane_bits, saturated);
				break;
			case 6:
			case 7: {
				const bool matches = unsigned_group ?
					(opcode == 6U ? first > second : first >= second) :
					(opcode == 6U ?
						first_signed > second_signed :
						first_signed >= second_signed);
				value = matches ? mask(lane_bits) : 0;
				break;
			}
			case 8:
			case 10: {
				const int shift = saturated_shift(second, lane_bits);
				const bool rounding = opcode == 10U;
				if (shift >= 0) {
					if (static_cast<unsigned>(shift) >= lane_bits) {
						value = 0;
					} else {
						const auto multiplier =
							static_cast<__int128>(1) << shift;
						value = unsigned_group ?
							truncate(static_cast<__int128>(first) *
								multiplier, lane_bits) :
							truncate(first_signed * multiplier, lane_bits);
					}
				} else if (unsigned_group) {
					value = static_cast<std::uint64_t>(
						shift_right_unsigned(first,
							static_cast<unsigned>(-shift), rounding));
				} else {
					value = truncate(shift_right_signed(first_signed,
						static_cast<unsigned>(-shift), rounding),
						lane_bits);
				}
				break;
			}
			case 9:
			case 11: {
				const int shift = saturated_shift(second, lane_bits);
				const bool rounding = opcode == 11U;
				if (shift >= 0) {
					if (static_cast<unsigned>(shift) >= lane_bits) {
						if (first == 0) {
							value = 0;
						} else if (unsigned_group) {
							value = saturate_unsigned(
								static_cast<__int128>(mask(lane_bits)) + 1,
								lane_bits, saturated);
						} else {
							const __int128 limit =
								static_cast<__int128>(1) <<
								(lane_bits - 1U);
							value = saturate_signed(first_signed < 0 ?
								-limit - 1 : limit,
								lane_bits, saturated);
						}
					} else {
						const auto multiplier =
							static_cast<__int128>(1) << shift;
						value = unsigned_group ?
							saturate_unsigned(
								static_cast<__int128>(first) * multiplier,
								lane_bits, saturated) :
							saturate_signed(first_signed * multiplier,
								lane_bits, saturated);
					}
				} else if (unsigned_group) {
					value = saturate_unsigned(
						static_cast<__int128>(shift_right_unsigned(
							first, static_cast<unsigned>(-shift),
							rounding)),
						lane_bits, saturated);
				} else {
					value = saturate_signed(
						shift_right_signed(first_signed,
							static_cast<unsigned>(-shift), rounding),
						lane_bits, saturated);
				}
				break;
			}
			case 12:
				value = unsigned_group ? std::max(first, second) :
					truncate(std::max(first_signed, second_signed),
						lane_bits);
				break;
			case 13:
				value = unsigned_group ? std::min(first, second) :
					truncate(std::min(first_signed, second_signed),
						lane_bits);
				break;
			case 14: {
				const __int128 difference = unsigned_group ?
					(first >= second ?
						static_cast<__int128>(first - second) :
						static_cast<__int128>(second - first)) :
					first_signed >= second_signed ?
						first_signed - second_signed :
						second_signed - first_signed;
				value = truncate(difference, lane_bits);
				break;
			}
			case 15: {
				const __int128 difference = unsigned_group ?
					(first >= second ?
						static_cast<__int128>(first - second) :
						static_cast<__int128>(second - first)) :
					first_signed >= second_signed ?
						first_signed - second_signed :
						second_signed - first_signed;
				value = truncate(static_cast<__int128>(old) + difference,
					lane_bits);
				break;
			}
			case 16:
				value = unsigned_group ? first - second : first + second;
				break;
			case 17:
				value = unsigned_group ?
					(first == second ? mask(lane_bits) : 0) :
					((first & second) != 0 ? mask(lane_bits) : 0);
				break;
			case 18:
				value = unsigned_group ?
					old - first * second :
					old + first * second;
				break;
			case 19:
				value = unsigned_group ?
					polynomial_multiply_byte(first, second) :
					first * second;
				break;
			case 22: {
				const __int128 product =
					first_signed * second_signed * 2;
				value = saturate_signed(
					shift_right_signed(product, lane_bits,
						unsigned_group),
					lane_bits, saturated);
				break;
			}
			default:
				break;
			}
			write(index, value);
		}
	}

	auto expected = initial;
	expected.vectors[registers.destination].bytes = result;
	if (saturated)
		expected.fpsr |= 1U << 27;
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::features(
		uml::backend::ArmFeature::advanced_simd);
}

} // namespace arm64_simd_three_same_test
