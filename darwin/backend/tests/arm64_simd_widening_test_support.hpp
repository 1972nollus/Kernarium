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

namespace arm64_simd_widening_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

enum class Operation : unsigned {
	add_long = 0U,
	add_wide = 1U,
	subtract_long = 2U,
	subtract_wide = 3U,
	absolute_accumulate = 5U,
	absolute_long = 7U,
};

inline constexpr std::array operations{
	Operation::add_long,
	Operation::add_wide,
	Operation::subtract_long,
	Operation::subtract_wide,
	Operation::absolute_accumulate,
	Operation::absolute_long,
};

inline constexpr std::array long_operations{
	Operation::add_long,
	Operation::subtract_long,
	Operation::absolute_long,
};

inline constexpr std::array wide_operations{
	Operation::add_wide,
	Operation::subtract_wide,
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

[[nodiscard]] constexpr unsigned encoding(Operation operation)
{
	return static_cast<unsigned>(operation);
}

[[nodiscard]] constexpr bool accumulates(Operation operation)
{
	return operation == Operation::absolute_accumulate;
}

[[nodiscard]] constexpr bool absolute_difference(Operation operation)
{
	return operation == Operation::absolute_accumulate ||
		operation == Operation::absolute_long;
}

[[nodiscard]] constexpr bool wide_left(Operation operation)
{
	return operation == Operation::add_wide ||
		operation == Operation::subtract_wide;
}

[[nodiscard]] constexpr bool subtracting(Operation operation)
{
	return operation == Operation::subtract_long ||
		operation == Operation::subtract_wide;
}

[[nodiscard]] constexpr std::string_view operation_name(Operation operation,
	bool unsigned_values)
{
	switch (operation) {
	case Operation::add_long: return unsigned_values ? "uaddl" : "saddl";
	case Operation::add_wide: return unsigned_values ? "uaddw" : "saddw";
	case Operation::subtract_long: return unsigned_values ? "usubl" : "ssubl";
	case Operation::subtract_wide: return unsigned_values ? "usubw" : "ssubw";
	case Operation::absolute_accumulate:
		return unsigned_values ? "uabal" : "sabal";
	case Operation::absolute_long: return unsigned_values ? "uabdl" : "sabdl";
	}
	return "unknown";
}

[[nodiscard]] constexpr std::uint32_t instruction(Operation operation,
	bool unsigned_values, Shape shape, Registers registers)
{
	return 0x0e20'0000U |
		(shape.upper ? 0x4000'0000U : 0U) |
		(unsigned_values ? 0x2000'0000U : 0U) |
		((shape.size & 3U) << 22) |
		(encoding(operation) << 12) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

/*
 * The scalar three-different allocation shares the operation field, but none
 * of the twelve vector instructions audited by this suite has a scalar form.
 */
[[nodiscard]] constexpr std::uint32_t scalar_instruction(
	Operation operation, bool unsigned_values, unsigned size,
	Registers registers)
{
	return 0x5e20'0000U |
		(unsigned_values ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) |
		(encoding(operation) << 12) |
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

[[nodiscard]] constexpr __int128 signed_integer(
	std::uint64_t raw, unsigned bits)
{
	raw &= mask(bits);
	if ((raw & (std::uint64_t{1} << (bits - 1U))) == 0)
		return static_cast<__int128>(raw);
	return static_cast<__int128>(raw) -
		(static_cast<__int128>(1) << bits);
}

[[nodiscard]] constexpr __int128 integer(
	std::uint64_t raw, unsigned bits, bool unsigned_values)
{
	return unsigned_values ? static_cast<__int128>(raw & mask(bits)) :
		signed_integer(raw, bits);
}

[[nodiscard]] constexpr std::uint64_t truncate(
	__int128 value, unsigned bits)
{
	return static_cast<std::uint64_t>(
		static_cast<unsigned __int128>(value)) & mask(bits);
}

/*
 * This is a direct mathematical transcription of the specification. It
 * deliberately does not reproduce the interpreter's unsigned wrapping
 * sequence and does not execute a host vector instruction.
 */
[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, Operation operation,
	bool unsigned_values, Shape shape, Registers registers)
{
	const auto left = initial.vectors[registers.left].bytes;
	const auto right = initial.vectors[registers.right].bytes;
	const auto old_destination =
		initial.vectors[registers.destination].bytes;
	std::array<std::byte, 16> result{};
	const unsigned narrow_bytes = 1U << shape.size;
	const unsigned wide_bytes = narrow_bytes * 2U;
	const unsigned narrow_bits = narrow_bytes * 8U;
	const unsigned wide_bits = wide_bytes * 8U;
	const unsigned source_base = shape.upper ? 8U : 0U;
	const unsigned lanes = 8U / narrow_bytes;

	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto right_raw = read_lane(
			right, narrow_bytes, source_base / narrow_bytes + lane);
		const auto right_value =
			integer(right_raw, narrow_bits, unsigned_values);
		__int128 value;

		if (absolute_difference(operation)) {
			const auto left_raw = read_lane(
				left, narrow_bytes, source_base / narrow_bytes + lane);
			auto difference =
				integer(left_raw, narrow_bits, unsigned_values) -
				right_value;
			if (difference < 0)
				difference = -difference;
			value = difference;
			if (accumulates(operation))
				value += static_cast<__int128>(
					read_lane(old_destination, wide_bytes, lane));
		} else {
			const auto left_raw = wide_left(operation) ?
				read_lane(left, wide_bytes, lane) :
				read_lane(left, narrow_bytes,
					source_base / narrow_bytes + lane);
			const auto left_value = integer(left_raw,
				wide_left(operation) ? wide_bits : narrow_bits,
				unsigned_values);
			value = subtracting(operation) ?
				left_value - right_value :
				left_value + right_value;
		}
		write_lane(result, wide_bytes, lane,
			truncate(value, wide_bits));
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

inline void require_engine_counts(
	const engine_tests::EngineHarness &harness, std::size_t total,
	std::size_t host_feature_cases)
{
	const auto &counts = harness.counts();
	const bool native_supported =
		engine_tests::host_supports(native_requirements());
	const std::size_t expected_feature_skips =
		native_supported ? 0U : host_feature_cases;
	require(counts.interpreter == total,
		"unexpected interpreter case count");
	require(counts.gadgets == total,
		"unexpected gadget case count");
	require(counts.jit == total - expected_feature_skips,
		"unexpected JIT execution count");
	require(counts.jit_skipped_host_feature == expected_feature_skips,
		"unexpected JIT host-feature skip count");
	require(counts.jit_skipped == expected_feature_skips,
		"unexpected total JIT skip count");
	require(counts.jit_unavailable == 0U,
		"native JIT engine is unavailable in this build");
}

} // namespace arm64_simd_widening_test
