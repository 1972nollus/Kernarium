// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <span>
#include <string_view>

namespace arm64_simd_indexed_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

enum class Encoding {
	indexed,
	vector_long,
	vector_rdm,
};

enum class Operation {
	mla,
	mls,
	mul,
	smlal,
	smlsl,
	smull,
	umlal,
	umlsl,
	umull,
	sqdmlal,
	sqdmlsl,
	sqdmull,
	sqdmulh,
	sqrdmulh,
	sqrdmlah,
	sqrdmlsh,
};

struct Family final {
	Operation operation;
	Encoding encoding;
};

struct Shape final {
	unsigned size;
	bool scalar;
	bool q;
};

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
	unsigned index{};
};

inline constexpr std::array indexed_same_families{
	Family{Operation::mla, Encoding::indexed},
	Family{Operation::mls, Encoding::indexed},
	Family{Operation::mul, Encoding::indexed},
};

inline constexpr std::array indexed_long_families{
	Family{Operation::smlal, Encoding::indexed},
	Family{Operation::smlsl, Encoding::indexed},
	Family{Operation::smull, Encoding::indexed},
	Family{Operation::umlal, Encoding::indexed},
	Family{Operation::umlsl, Encoding::indexed},
	Family{Operation::umull, Encoding::indexed},
};

inline constexpr std::array vector_long_families{
	Family{Operation::smlal, Encoding::vector_long},
	Family{Operation::smlsl, Encoding::vector_long},
	Family{Operation::smull, Encoding::vector_long},
	Family{Operation::umlal, Encoding::vector_long},
	Family{Operation::umlsl, Encoding::vector_long},
	Family{Operation::umull, Encoding::vector_long},
};

inline constexpr std::array indexed_saturating_long_families{
	Family{Operation::sqdmlal, Encoding::indexed},
	Family{Operation::sqdmlsl, Encoding::indexed},
	Family{Operation::sqdmull, Encoding::indexed},
};

inline constexpr std::array vector_saturating_long_families{
	Family{Operation::sqdmlal, Encoding::vector_long},
	Family{Operation::sqdmlsl, Encoding::vector_long},
	Family{Operation::sqdmull, Encoding::vector_long},
};

inline constexpr std::array indexed_saturating_high_families{
	Family{Operation::sqdmulh, Encoding::indexed},
	Family{Operation::sqrdmulh, Encoding::indexed},
};

inline constexpr std::array indexed_rdm_families{
	Family{Operation::sqrdmlah, Encoding::indexed},
	Family{Operation::sqrdmlsh, Encoding::indexed},
};

inline constexpr std::array vector_rdm_families{
	Family{Operation::sqrdmlah, Encoding::vector_rdm},
	Family{Operation::sqrdmlsh, Encoding::vector_rdm},
};

inline constexpr std::array all_families{
	Family{Operation::mla, Encoding::indexed},
	Family{Operation::mls, Encoding::indexed},
	Family{Operation::mul, Encoding::indexed},
	Family{Operation::smlal, Encoding::indexed},
	Family{Operation::smlsl, Encoding::indexed},
	Family{Operation::smull, Encoding::indexed},
	Family{Operation::smlal, Encoding::vector_long},
	Family{Operation::smlsl, Encoding::vector_long},
	Family{Operation::smull, Encoding::vector_long},
	Family{Operation::sqdmlal, Encoding::indexed},
	Family{Operation::sqdmlsl, Encoding::indexed},
	Family{Operation::sqdmlal, Encoding::vector_long},
	Family{Operation::sqdmlsl, Encoding::vector_long},
	Family{Operation::sqdmulh, Encoding::indexed},
	Family{Operation::sqdmull, Encoding::indexed},
	Family{Operation::sqdmull, Encoding::vector_long},
	Family{Operation::sqrdmlah, Encoding::indexed},
	Family{Operation::sqrdmlah, Encoding::vector_rdm},
	Family{Operation::sqrdmlsh, Encoding::indexed},
	Family{Operation::sqrdmlsh, Encoding::vector_rdm},
	Family{Operation::sqrdmulh, Encoding::indexed},
	Family{Operation::umlal, Encoding::indexed},
	Family{Operation::umlal, Encoding::vector_long},
	Family{Operation::umlsl, Encoding::indexed},
	Family{Operation::umlsl, Encoding::vector_long},
	Family{Operation::umull, Encoding::indexed},
	Family{Operation::umull, Encoding::vector_long},
};

inline constexpr std::array indexed_vector_shapes{
	Shape{1U, false, false}, Shape{1U, false, true},
	Shape{2U, false, false}, Shape{2U, false, true},
};

inline constexpr std::array vector_long_shapes{
	Shape{0U, false, false}, Shape{0U, false, true},
	Shape{1U, false, false}, Shape{1U, false, true},
	Shape{2U, false, false}, Shape{2U, false, true},
};

inline constexpr std::array saturating_shapes{
	Shape{1U, false, false}, Shape{1U, false, true},
	Shape{2U, false, false}, Shape{2U, false, true},
	Shape{1U, true, false}, Shape{2U, true, false},
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

[[nodiscard]] constexpr bool is_unsigned(Operation operation) noexcept
{
	return operation == Operation::umlal ||
		operation == Operation::umlsl ||
		operation == Operation::umull;
}

[[nodiscard]] constexpr bool is_same_width(Operation operation) noexcept
{
	return operation == Operation::mla ||
		operation == Operation::mls ||
		operation == Operation::mul;
}

[[nodiscard]] constexpr bool is_normal_long(Operation operation) noexcept
{
	return operation == Operation::smlal ||
		operation == Operation::smlsl ||
		operation == Operation::smull ||
		operation == Operation::umlal ||
		operation == Operation::umlsl ||
		operation == Operation::umull;
}

[[nodiscard]] constexpr bool is_saturating_long(
	Operation operation) noexcept
{
	return operation == Operation::sqdmlal ||
		operation == Operation::sqdmlsl ||
		operation == Operation::sqdmull;
}

[[nodiscard]] constexpr bool is_saturating_high(
	Operation operation) noexcept
{
	return operation == Operation::sqdmulh ||
		operation == Operation::sqrdmulh;
}

[[nodiscard]] constexpr bool is_rdm(Operation operation) noexcept
{
	return operation == Operation::sqrdmlah ||
		operation == Operation::sqrdmlsh;
}

[[nodiscard]] constexpr bool accumulates(Operation operation) noexcept
{
	return operation == Operation::mla ||
		operation == Operation::mls ||
		operation == Operation::smlal ||
		operation == Operation::smlsl ||
		operation == Operation::umlal ||
		operation == Operation::umlsl ||
		operation == Operation::sqdmlal ||
		operation == Operation::sqdmlsl ||
		is_rdm(operation);
}

[[nodiscard]] constexpr bool subtracts(Operation operation) noexcept
{
	return operation == Operation::mls ||
		operation == Operation::smlsl ||
		operation == Operation::umlsl ||
		operation == Operation::sqdmlsl ||
		operation == Operation::sqrdmlsh;
}

[[nodiscard]] constexpr bool rounded(Operation operation) noexcept
{
	return operation == Operation::sqrdmulh || is_rdm(operation);
}

[[nodiscard]] constexpr bool indexed(const Family &family) noexcept
{
	return family.encoding == Encoding::indexed;
}

[[nodiscard]] constexpr unsigned indexed_operation(
	Operation operation) noexcept
{
	switch (operation) {
	case Operation::mla: return 0U;
	case Operation::mls: return 4U;
	case Operation::mul: return 8U;
	case Operation::smlal:
	case Operation::umlal: return 2U;
	case Operation::smlsl:
	case Operation::umlsl: return 6U;
	case Operation::smull:
	case Operation::umull: return 10U;
	case Operation::sqdmlal: return 3U;
	case Operation::sqdmlsl: return 7U;
	case Operation::sqdmull: return 11U;
	case Operation::sqdmulh: return 12U;
	case Operation::sqrdmulh:
	case Operation::sqrdmlah: return 13U;
	case Operation::sqrdmlsh: return 15U;
	}
	return 0U;
}

[[nodiscard]] constexpr bool indexed_unsigned_group(
	Operation operation) noexcept
{
	return operation == Operation::mla ||
		operation == Operation::mls ||
		is_unsigned(operation) ||
		is_rdm(operation);
}

[[nodiscard]] constexpr unsigned vector_long_operation(
	Operation operation) noexcept
{
	switch (operation) {
	case Operation::smlal:
	case Operation::umlal: return 8U;
	case Operation::sqdmlal: return 9U;
	case Operation::smlsl:
	case Operation::umlsl: return 10U;
	case Operation::sqdmlsl: return 11U;
	case Operation::smull:
	case Operation::umull: return 12U;
	case Operation::sqdmull: return 13U;
	default: return 0U;
	}
}

[[nodiscard]] constexpr std::uint32_t instruction(const Family &family,
	Shape shape, Registers registers) noexcept
{
	if (family.encoding == Encoding::vector_rdm)
		return (shape.scalar ? 0x7e00'8400U : 0x2e00'8400U) |
			(!shape.scalar && shape.q ? 0x4000'0000U : 0U) |
			(subtracts(family.operation) ? 0x800U : 0U) |
			((shape.size & 3U) << 22) |
			((registers.right & 31U) << 16) |
			((registers.left & 31U) << 5) |
			(registers.destination & 31U);
	if (family.encoding == Encoding::vector_long)
		return (shape.scalar ? 0x5e20'0000U : 0x0e20'0000U) |
			(!shape.scalar && shape.q ? 0x4000'0000U : 0U) |
			(is_unsigned(family.operation) ? 0x2000'0000U : 0U) |
			((shape.size & 3U) << 22) |
			(vector_long_operation(family.operation) << 12) |
			((registers.right & 31U) << 16) |
			((registers.left & 31U) << 5) |
			(registers.destination & 31U);

	std::uint32_t encoded =
		(shape.scalar ? 0x5f00'0000U : 0x0f00'0000U) |
		(!shape.scalar && shape.q ? 0x4000'0000U : 0U) |
		(indexed_unsigned_group(family.operation) ?
			0x2000'0000U : 0U) |
		((shape.size & 3U) << 22) |
		(indexed_operation(family.operation) << 12) |
		((registers.right & 15U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
	if (shape.size == 1U) {
		encoded |= (registers.index & 1U) << 20;
		encoded |= (registers.index & 2U) << 20;
		encoded |= (registers.index & 4U) << 9;
	} else if (shape.size == 2U) {
		encoded |= ((registers.right >> 4) & 1U) << 20;
		encoded |= (registers.index & 1U) << 21;
		encoded |= (registers.index & 2U) << 10;
	}
	return encoded;
}

[[nodiscard]] constexpr std::span<const Shape>
allocated_shapes(const Family &family) noexcept
{
	if (is_same_width(family.operation) ||
	    (is_normal_long(family.operation) && indexed(family)))
		return indexed_vector_shapes;
	if (is_normal_long(family.operation))
		return vector_long_shapes;
	return saturating_shapes;
}

[[nodiscard]] constexpr unsigned lane_count(
	const Family &family, Shape shape) noexcept
{
	return indexed(family) ? (shape.size == 1U ? 8U : 4U) : 1U;
}

[[nodiscard]] constexpr unsigned right_register_count(
	const Family &family, Shape shape) noexcept
{
	return indexed(family) && shape.size == 1U ? 16U : 32U;
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
	cpu.fpcr = 0;
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

[[nodiscard]] constexpr std::uint64_t mask(unsigned bits) noexcept
{
	return bits == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << bits) - 1U;
}

[[nodiscard]] inline std::uint64_t read_lane(
	const std::array<std::byte, 16> &bytes, unsigned lane_bytes,
	unsigned lane) noexcept
{
	std::uint64_t value = 0;
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(
				bytes[lane * lane_bytes + byte])) << (byte * 8U);
	return value;
}

inline void write_lane(std::array<std::byte, 16> &bytes,
	unsigned lane_bytes, unsigned lane, std::uint64_t value) noexcept
{
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		bytes[lane * lane_bytes + byte] =
			static_cast<std::byte>(value >> (byte * 8U));
}

inline void fill_lanes(arm64::CpuState &cpu, unsigned reg,
	unsigned lane_bytes, std::uint64_t value) noexcept
{
	for (unsigned lane = 0; lane < 16U / lane_bytes; ++lane)
		write_lane(cpu.vectors[reg].bytes, lane_bytes, lane, value);
}

inline void set_lane(arm64::CpuState &cpu, unsigned reg,
	unsigned lane_bytes, unsigned lane, std::uint64_t value) noexcept
{
	write_lane(cpu.vectors[reg].bytes, lane_bytes, lane, value);
}

[[nodiscard]] constexpr __int128 signed_value(
	std::uint64_t raw, unsigned bits) noexcept
{
	raw &= mask(bits);
	if ((raw & (std::uint64_t{1} << (bits - 1U))) == 0)
		return static_cast<__int128>(raw);
	return static_cast<__int128>(raw) -
		(static_cast<__int128>(1) << bits);
}

[[nodiscard]] constexpr __int128 floor_shift_right(
	__int128 value, unsigned shift) noexcept
{
	if (value >= 0)
		return value >> shift;
	const __int128 divisor = static_cast<__int128>(1) << shift;
	return -((-value + divisor - 1) / divisor);
}

[[nodiscard]] inline std::uint64_t saturate_signed(
	__int128 value, unsigned bits, bool &saturated) noexcept
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
	return static_cast<std::uint64_t>(
		static_cast<unsigned __int128>(value)) & mask(bits);
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, const Family &family, Shape shape,
	Registers registers)
{
	const auto old_destination =
		initial.vectors[registers.destination].bytes;
	const auto left = initial.vectors[registers.left].bytes;
	const auto right = initial.vectors[registers.right].bytes;
	std::array<std::byte, 16> result{};
	const unsigned narrow_bytes = 1U << shape.size;
	const unsigned narrow_bits = narrow_bytes * 8U;
	bool saturated = false;

	if (is_same_width(family.operation)) {
		const unsigned bytes = shape.q ? 16U : 8U;
		for (unsigned lane = 0; lane < bytes / narrow_bytes; ++lane) {
			const auto left_value =
				read_lane(left, narrow_bytes, lane);
			const auto right_value = read_lane(
				right, narrow_bytes, registers.index);
			unsigned __int128 value =
				static_cast<unsigned __int128>(left_value) *
				right_value;
			const auto accumulator =
				read_lane(old_destination, narrow_bytes, lane);
			if (family.operation == Operation::mla)
				value += accumulator;
			else if (family.operation == Operation::mls)
				value = static_cast<unsigned __int128>(
					accumulator) - value;
			write_lane(result, narrow_bytes, lane,
				static_cast<std::uint64_t>(value) &
					mask(narrow_bits));
		}
	} else if (is_normal_long(family.operation) ||
		   is_saturating_long(family.operation)) {
		const unsigned wide_bytes = narrow_bytes * 2U;
		const unsigned wide_bits = narrow_bits * 2U;
		const unsigned output_bytes = shape.scalar ? wide_bytes : 16U;
		const unsigned source_lane_base =
			!shape.scalar && shape.q ? 8U / narrow_bytes : 0U;
		for (unsigned lane = 0; lane < output_bytes / wide_bytes;
		     ++lane) {
			const auto left_raw = read_lane(
				left, narrow_bytes, source_lane_base + lane);
			const auto right_raw = indexed(family) ?
				read_lane(right, narrow_bytes, registers.index) :
				read_lane(right, narrow_bytes,
					source_lane_base + lane);
			std::uint64_t value;
			if (is_saturating_long(family.operation)) {
				const auto product =
					signed_value(left_raw, narrow_bits) *
					signed_value(right_raw, narrow_bits) * 2;
				const auto product_raw = saturate_signed(
					product, wide_bits, saturated);
				__int128 combined =
					signed_value(product_raw, wide_bits);
				if (accumulates(family.operation)) {
					combined = signed_value(read_lane(
						old_destination, wide_bytes, lane),
						wide_bits) +
						(subtracts(family.operation) ?
						 -combined : combined);
				}
				value = saturate_signed(
					combined, wide_bits, saturated);
			} else {
				const __int128 left_value =
					is_unsigned(family.operation) ?
						static_cast<__int128>(left_raw) :
						signed_value(left_raw, narrow_bits);
				const __int128 right_value =
					is_unsigned(family.operation) ?
						static_cast<__int128>(right_raw) :
						signed_value(right_raw, narrow_bits);
				const auto product = static_cast<unsigned __int128>(
					left_value * right_value);
				unsigned __int128 combined = product;
				if (accumulates(family.operation)) {
					const auto accumulator = read_lane(
						old_destination, wide_bytes, lane);
					combined = subtracts(family.operation) ?
						static_cast<unsigned __int128>(
							accumulator) - product :
						static_cast<unsigned __int128>(
							accumulator) + product;
				}
				value = static_cast<std::uint64_t>(
					combined) & mask(wide_bits);
			}
			write_lane(result, wide_bytes, lane, value);
		}
	} else {
		const unsigned output_bytes =
			shape.scalar ? narrow_bytes : shape.q ? 16U : 8U;
		for (unsigned lane = 0; lane < output_bytes / narrow_bytes;
		     ++lane) {
			const auto left_value = signed_value(
				read_lane(left, narrow_bytes, lane), narrow_bits);
			const auto right_value = signed_value(
				indexed(family) ?
					read_lane(right, narrow_bytes,
						registers.index) :
					read_lane(right, narrow_bytes, lane),
				narrow_bits);
			const __int128 product =
				left_value * right_value * 2;
			__int128 combined;
			if (is_rdm(family.operation)) {
				combined = signed_value(read_lane(
					old_destination, narrow_bytes, lane),
					narrow_bits) *
					(static_cast<__int128>(1) << narrow_bits);
				combined += subtracts(family.operation) ?
					-product : product;
			} else {
				combined = product;
			}
			if (rounded(family.operation))
				combined += static_cast<__int128>(1) <<
					(narrow_bits - 1U);
			const auto shifted =
				floor_shift_right(combined, narrow_bits);
			write_lane(result, narrow_bytes, lane,
				saturate_signed(
					shifted, narrow_bits, saturated));
		}
	}

	auto expected = initial;
	expected.vectors[registers.destination].bytes = result;
	if (saturated)
		expected.fpsr |= 1U << 27;
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(const Family &family)
{
	using uml::backend::ArmFeature;
	auto requirements = engine_tests::NativeRequirements::features(
		ArmFeature::advanced_simd);
	if (is_rdm(family.operation))
		requirements = requirements | ArmFeature::rdm;
	return requirements;
}

struct ExpectedCounts final {
	std::size_t cases{};
	std::size_t jit{};
	std::size_t feature_skips{};

	void add(engine_tests::NativeRequirements requirements,
		bool invalid = false)
	{
		++cases;
		if (invalid || engine_tests::host_supports(requirements))
			++jit;
		else
			++feature_skips;
	}
};

inline void require_counts(const engine_tests::EngineHarness &harness,
	const ExpectedCounts &expected)
{
	const auto &actual = harness.counts();
	require(actual.interpreter == expected.cases,
		"unexpected interpreter case count");
	require(actual.gadgets == expected.cases,
		"unexpected gadget case count");
	require(actual.jit == expected.jit,
		"unexpected JIT case count");
	require(actual.jit_skipped == expected.feature_skips,
		"unexpected JIT skip count");
	require(actual.jit_skipped_host_feature == expected.feature_skips,
		"unexpected JIT host-feature skip count");
	require(actual.jit_unavailable == 0U,
		"native JIT engine unavailable");
}

inline void check_one(engine_tests::EngineHarness &harness,
	std::string_view name, const Family &family, Shape shape,
	Registers registers, const arm64::CpuState &initial,
	ExpectedCounts &counts)
{
	const auto requirements = native_requirements(family);
	harness.check_one(name, instruction(family, shape, registers),
		initial, expected_state(initial, family, shape, registers),
		requirements);
	counts.add(requirements);
}

inline void check_illegal(engine_tests::EngineHarness &harness,
	std::string_view name, const Family &family, Shape shape,
	Registers registers, const arm64::CpuState &initial,
	ExpectedCounts &counts)
{
	const auto requirements = native_requirements(family);
	harness.check_illegal(name, instruction(family, shape, registers),
		initial, requirements);
	counts.add(requirements, true);
}

} // namespace arm64_simd_indexed_test
