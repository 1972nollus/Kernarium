// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_fcmp_test_support.hpp"

#include <array>
#include <span>
#include <string_view>

namespace arm64_simd_floating_compare_test {

namespace base = arm64_simd_test;
namespace move = arm64_simd_fmov_test;
namespace scalar = arm64_simd_fcmp_test;
namespace engine_tests = uml::emulator::tests;

enum class Operation : unsigned {
	equal,
	greater_equal,
	greater,
	less_equal,
	less,
	absolute_greater_equal,
	absolute_greater,
};

inline constexpr std::array register_operations{
	Operation::equal, Operation::greater_equal, Operation::greater,
	Operation::absolute_greater_equal, Operation::absolute_greater};
inline constexpr std::array zero_operations{
	Operation::equal, Operation::greater_equal, Operation::greater,
	Operation::less_equal, Operation::less};

struct Shape final {
	unsigned width;
	unsigned vector_bytes;
	bool scalar;
	bool q;
};

inline constexpr std::array shapes{
	Shape{16U, 2U, true, false}, Shape{32U, 4U, true, false},
	Shape{64U, 8U, true, false}, Shape{16U, 8U, false, false},
	Shape{16U, 16U, false, true}, Shape{32U, 8U, false, false},
	Shape{32U, 16U, false, true}, Shape{64U, 16U, false, true},
};

[[nodiscard]] constexpr const scalar::Format &format_for_bytes(unsigned bytes)
{
	return scalar::format_for_type(bytes == 2U ? 3U : bytes == 4U ? 0U : 1U);
}

[[nodiscard]] constexpr std::uint64_t positive_one(unsigned bytes)
{
	return format_for_bytes(bytes).positive_one;
}

[[nodiscard]] constexpr std::uint64_t positive_two(unsigned bytes)
{
	return format_for_bytes(bytes).positive_two;
}

[[nodiscard]] constexpr std::uint64_t negative_one(unsigned bytes)
{
	const auto &format = format_for_bytes(bytes);
	return format.sign | format.positive_one;
}

[[nodiscard]] constexpr std::uint64_t negative_zero(unsigned bytes)
{
	return format_for_bytes(bytes).sign;
}

[[nodiscard]] constexpr std::uint64_t quiet_nan(unsigned bytes)
{
	return format_for_bytes(bytes).quiet_nan;
}

[[nodiscard]] constexpr std::uint64_t signaling_nan(unsigned bytes)
{
	return format_for_bytes(bytes).signaling_nan;
}

[[nodiscard]] constexpr std::uint32_t register_instruction(Operation operation,
	const Shape &shape, unsigned destination, unsigned left, unsigned right)
{
	const bool half = shape.width == 16U;
	std::uint32_t encoded = half ?
		(shape.scalar ? 0x5e40'2400U : 0x0e40'2400U) :
		(shape.scalar ? 0x5e20'e400U : 0x0e20'e400U);
	if (!shape.scalar && shape.q)
		encoded |= 0x4000'0000U;
	if (!half && shape.width == 64U)
		encoded |= 0x0040'0000U;
	if (operation != Operation::equal)
		encoded |= 0x2000'0000U;
	if (operation == Operation::greater ||
	    operation == Operation::absolute_greater)
		encoded |= 0x0080'0000U;
	if (operation == Operation::absolute_greater_equal ||
	    operation == Operation::absolute_greater)
		encoded |= 0x0000'0800U;
	return encoded | ((right & 31U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t zero_instruction(Operation operation,
	const Shape &shape, unsigned destination, unsigned source)
{
	const bool half = shape.width == 16U;
	std::uint32_t encoded = half ?
		(shape.scalar ? 0x5ef8'd800U : 0x0ef8'd800U) :
		(shape.scalar ? 0x5ea0'd800U : 0x0ea0'd800U);
	if (!shape.scalar && shape.q)
		encoded |= 0x4000'0000U;
	if (!half && shape.width == 64U)
		encoded |= 0x0040'0000U;
	switch (operation) {
	case Operation::equal: break;
	case Operation::greater_equal: encoded = (encoded - 0x1000U) |
		0x2000'0000U; break;
	case Operation::greater: encoded -= 0x1000U; break;
	case Operation::less_equal: encoded |= 0x2000'0000U; break;
	case Operation::less: encoded += 0x1000U; break;
	case Operation::absolute_greater_equal:
	case Operation::absolute_greater:
		break;
	}
	return encoded | ((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint64_t lane_mask(unsigned width)
{
	return width == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << width) - 1U;
}

inline void write_lane(std::array<std::byte, 16> &bytes, const Shape &shape,
	unsigned lane, std::uint64_t bits)
{
	move::write_bits(bytes, lane * shape.width / 8U, shape.width / 8U, bits);
}

[[nodiscard]] constexpr bool comparison_matches(
	Operation operation, std::uint32_t nzcv) noexcept
{
	if (operation == Operation::equal)
		return nzcv == 0x6000'0000U;
	if (operation == Operation::greater_equal ||
	    operation == Operation::less_equal ||
	    operation == Operation::absolute_greater_equal)
		return nzcv == 0x6000'0000U || nzcv == 0x2000'0000U;
	return nzcv == 0x2000'0000U;
}

[[nodiscard]] constexpr bool ordered_match(Operation operation,
	int relation) noexcept
{
	switch (operation) {
	case Operation::equal: return relation == 0;
	case Operation::greater_equal: return relation >= 0;
	case Operation::greater: return relation > 0;
	case Operation::less_equal: return relation <= 0;
	case Operation::less: return relation < 0;
	case Operation::absolute_greater_equal: return relation >= 0;
	case Operation::absolute_greater: return relation > 0;
	}
	return false;
}

[[nodiscard]] inline uml::cpu::arm64::CpuState expected_state(
	const Shape &shape, Operation operation, bool zero,
	unsigned destination, unsigned left, unsigned right,
	const uml::cpu::arm64::CpuState &initial)
{
	const auto &format = scalar::format_for_type(
		shape.width == 16U ? 3U : shape.width == 32U ? 0U : 1U);
	auto expected = initial;
	std::array<std::byte, 16> result{};
	if (shape.scalar && !zero && (initial.fpcr & 4U) != 0U)
		result = initial.vectors[right].bytes;
	std::uint32_t exceptions = 0U;
	const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		auto first = move::read_bits(initial.vectors[left].bytes,
			lane * shape.width / 8U, shape.width / 8U);
		auto second = zero ? 0U : move::read_bits(
			initial.vectors[right].bytes, lane * shape.width / 8U,
			shape.width / 8U);
		const bool absolute =
			operation == Operation::absolute_greater_equal ||
			operation == Operation::absolute_greater;
		const bool reverse = operation == Operation::less_equal ||
			operation == Operation::less;
		if (absolute) {
			first &= ~format.sign;
			second &= ~format.sign;
		}
		const auto comparison = scalar::compare(
			reverse ? second : first, reverse ? first : second,
			format, operation != Operation::equal, initial.fpcr);
		exceptions |= comparison.exceptions;
		write_lane(result, shape, lane,
			comparison_matches(operation, comparison.nzcv) ?
				lane_mask(shape.width) : 0U);
	}
	expected.vectors[destination].bytes = result;
	expected.fpsr |= exceptions;
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(const Shape &shape, std::uint32_t fpcr)
{
	using ::uml::cpu::ArmFeature;
	auto requirements = engine_tests::NativeRequirements::features(
		ArmFeature::advanced_simd);
	if (shape.width == 16U)
		requirements = requirements | ArmFeature::fp16;
	if ((fpcr & 7U) != 0U)
		requirements = requirements | ArmFeature::afp;
	return requirements;
}

inline void check_result(engine_tests::EngineHarness &harness,
	std::string_view name, const Shape &shape, Operation operation,
	bool zero, unsigned destination, unsigned left, unsigned right,
	const uml::cpu::arm64::CpuState &initial)
{
	const auto encoded = zero ?
		zero_instruction(operation, shape, destination, left) :
		register_instruction(operation, shape, destination, left, right);
	harness.check_one(name, encoded, initial,
		expected_state(shape, operation, zero, destination, left, right,
			initial),
		native_requirements(shape, initial.fpcr));
}

} // namespace arm64_simd_floating_compare_test
