// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string_view>

namespace arm64_simd_high_narrow_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

enum class Operation : unsigned {
	add,
	rounded_add,
	subtract,
	rounded_subtract,
};

inline constexpr std::array operations{
	Operation::add,
	Operation::rounded_add,
	Operation::subtract,
	Operation::rounded_subtract,
};

struct Shape final {
	unsigned size;
	bool upper;
};

inline constexpr std::array shapes{
	Shape{0U, false}, Shape{1U, false}, Shape{2U, false},
	Shape{0U, true}, Shape{1U, true}, Shape{2U, true},
};

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

[[noreturn]] inline void fail(const char *message,
	const std::source_location location = std::source_location::current())
{
	std::fprintf(stderr, "%s at %s:%u\n", message,
		location.file_name(), location.line());
	std::abort();
}

inline void require(bool condition, const char *message = "require failed",
	const std::source_location location = std::source_location::current())
{
	if (!condition)
		fail(message, location);
}

[[nodiscard]] constexpr bool rounding(Operation operation)
{
	return operation == Operation::rounded_add ||
		operation == Operation::rounded_subtract;
}

[[nodiscard]] constexpr bool subtracting(Operation operation)
{
	return operation == Operation::subtract ||
		operation == Operation::rounded_subtract;
}

[[nodiscard]] constexpr std::string_view operation_name(Operation operation)
{
	switch (operation) {
	case Operation::add: return "addhn";
	case Operation::rounded_add: return "raddhn";
	case Operation::subtract: return "subhn";
	case Operation::rounded_subtract: return "rsubhn";
	}
	return "unknown";
}

[[nodiscard]] constexpr std::uint32_t instruction(Operation operation,
	Shape shape, Registers registers)
{
	return 0x0e20'4000U | (shape.upper ? 0x4000'0000U : 0U) |
		(rounding(operation) ? 0x2000'0000U : 0U) |
		((shape.size & 3U) << 22) |
		(subtracting(operation) ? 0x2000U : 0U) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
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
	cpu.fpcr = 0x0040'0000U ^ ((seed & 3U) << 22);
	cpu.fpsr = 0x0800'009fU ^ seed;
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
	return cpu;
}

[[nodiscard]] constexpr std::uint64_t mask(unsigned bits)
{
	return bits == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << bits) - 1U;
}

[[nodiscard]] inline std::uint64_t read_lane(
	const std::array<std::byte, 16> &bytes, unsigned lane_bytes,
	unsigned lane)
{
	std::uint64_t value = 0;
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(
			bytes[lane * lane_bytes + byte])) << (byte * 8U);
	return value;
}

inline void write_lane(std::array<std::byte, 16> &bytes,
	unsigned lane_bytes, unsigned lane, std::uint64_t value)
{
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		bytes[lane * lane_bytes + byte] =
			static_cast<std::byte>(value >> (byte * 8U));
}

[[nodiscard]] constexpr __int128 floor_divide_power_of_two(
	__int128 value, unsigned shift)
{
	const __int128 divisor = static_cast<__int128>(1) << shift;
	if (value >= 0)
		return value / divisor;
	return -((-value + divisor - 1) / divisor);
}

/*
 * This is deliberately a mathematical lane oracle.  It does not reproduce
 * the interpreter's fixed-width wrapping sequence or execute a host vector
 * instruction.  The final mask models assignment to the narrow result lane.
 */
[[nodiscard]] constexpr std::uint64_t oracle_lane(std::uint64_t left,
	std::uint64_t right, unsigned destination_bits, bool subtract,
	bool round)
{
	__int128 value = static_cast<__int128>(left);
	value += subtract ? -static_cast<__int128>(right) :
		static_cast<__int128>(right);
	if (round)
		value += static_cast<__int128>(1) << (destination_bits - 1U);
	const auto shifted = floor_divide_power_of_two(value, destination_bits);
	return static_cast<std::uint64_t>(
		static_cast<unsigned __int128>(shifted)) & mask(destination_bits);
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, Operation operation, Shape shape,
	Registers registers)
{
	const auto left = initial.vectors[registers.left].bytes;
	const auto right = initial.vectors[registers.right].bytes;
	const auto old_destination =
		initial.vectors[registers.destination].bytes;
	std::array<std::byte, 16> result{};
	const unsigned source_bytes = 2U << shape.size;
	const unsigned destination_bytes = source_bytes / 2U;
	const unsigned destination_bits = destination_bytes * 8U;
	if (shape.upper)
		std::copy_n(old_destination.begin(), 8U, result.begin());
	const unsigned output_base = shape.upper ? 8U : 0U;
	for (unsigned lane = 0; lane < 16U / source_bytes; ++lane) {
		const auto value = oracle_lane(
			read_lane(left, source_bytes, lane),
			read_lane(right, source_bytes, lane),
			destination_bits, subtracting(operation),
			rounding(operation));
		write_lane(result, destination_bytes,
			output_base / destination_bytes + lane, value);
	}
	auto expected = initial;
	expected.vectors[registers.destination].bytes = result;
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::features(
		uml::backend::ArmFeature::advanced_simd);
}

} // namespace arm64_simd_high_narrow_test
