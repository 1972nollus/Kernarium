// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_basic_floating_test_support.hpp"
#include "arm64_engine_test_support.hpp"
#include "cpu/arm64/detail/floating_horizontal.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

namespace arm64_floating_horizontal_test {

namespace basic = arm64_basic_floating_test;
namespace common = arm64_simd_test;
namespace arm64 = uml::backend::arm64;
namespace detail = uml::backend::arm64::detail;
namespace engine_tests = uml::backend::tests;

using Operation = detail::FloatingHorizontalOperation;
using Kind = detail::FloatingHorizontalForm;

struct Form final {
	Operation operation;
	Kind kind;
	unsigned width;
	unsigned vector_bytes;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array minmax_operations{
	Operation::maximum, Operation::maximum_number,
	Operation::minimum, Operation::minimum_number};
inline constexpr std::array register_cases{
	Registers{0, 1, 2}, Registers{1, 1, 2}, Registers{2, 1, 2},
	Registers{1, 1, 1}, Registers{31, 30, 29}, Registers{30, 31, 30},
	Registers{30, 15, 30}};

template<typename Function>
void for_each_form(Function &&function)
{
	for (const auto width : basic::widths)
		function(Form{Operation::absolute_difference, Kind::scalar_binary,
			width, 16U});
	for (const auto shape : basic::vector_shapes)
		function(Form{Operation::absolute_difference, Kind::vector_binary,
			shape[0], shape[1]});
	for (const auto width : basic::widths)
		function(Form{Operation::add, Kind::scalar_pairwise, width, 16U});
	for (const auto shape : basic::vector_shapes)
		function(Form{Operation::add, Kind::vector_pairwise,
			shape[0], shape[1]});
	for (const auto operation : minmax_operations) {
		for (const auto width : basic::widths)
			function(Form{operation, Kind::scalar_binary, width, 16U});
		for (const auto shape : basic::vector_shapes)
			function(Form{operation, Kind::vector_binary,
				shape[0], shape[1]});
		for (const auto width : basic::widths)
			function(Form{operation, Kind::scalar_pairwise, width, 16U});
		for (const auto shape : basic::vector_shapes)
			function(Form{operation, Kind::vector_pairwise,
				shape[0], shape[1]});
		for (const auto shape : {std::array{16U, 8U},
			std::array{16U, 16U}, std::array{32U, 16U}})
			function(Form{operation, Kind::reduction, shape[0], shape[1]});
	}
}

[[nodiscard]] constexpr unsigned operation_index(Operation operation) noexcept
{
	return operation == Operation::maximum ? 0U :
		operation == Operation::maximum_number ? 1U :
		operation == Operation::minimum ? 2U : 3U;
}

[[nodiscard]] constexpr unsigned scalar_shape(unsigned width) noexcept
{
	return width == 16U ? 0U : width == 32U ? 1U : 2U;
}

[[nodiscard]] constexpr unsigned vector_shape(unsigned width,
	unsigned vector_bytes) noexcept
{
	return width == 16U ? (vector_bytes == 16U ? 1U : 0U) :
		width == 32U ? (vector_bytes == 16U ? 3U : 2U) : 4U;
}

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2}) noexcept
{
	constexpr std::array fabd_scalar{0x7ec0'1400U, 0x7ea0'd400U,
		0x7ee0'd400U};
	constexpr std::array fabd_vector{0x2ec0'1400U, 0x6ec0'1400U,
		0x2ea0'd400U, 0x6ea0'd400U, 0x6ee0'd400U};
	constexpr std::array faddp_scalar{0x5e30'd800U, 0x7e30'd800U,
		0x7e70'd800U};
	constexpr std::array faddp_vector{0x2e40'1400U, 0x6e40'1400U,
		0x2e20'd400U, 0x6e20'd400U, 0x6e60'd400U};
	constexpr std::array minmax_scalar_binary{
		std::array{0x1ee0'4800U, 0x1e20'4800U, 0x1e60'4800U},
		std::array{0x1ee0'6800U, 0x1e20'6800U, 0x1e60'6800U},
		std::array{0x1ee0'5800U, 0x1e20'5800U, 0x1e60'5800U},
		std::array{0x1ee0'7800U, 0x1e20'7800U, 0x1e60'7800U}};
	constexpr std::array minmax_vector_binary{
		std::array{0x0e40'3400U, 0x4e40'3400U, 0x0e20'f400U,
			0x4e20'f400U, 0x4e60'f400U},
		std::array{0x0e40'0400U, 0x4e40'0400U, 0x0e20'c400U,
			0x4e20'c400U, 0x4e60'c400U},
		std::array{0x0ec0'3400U, 0x4ec0'3400U, 0x0ea0'f400U,
			0x4ea0'f400U, 0x4ee0'f400U},
		std::array{0x0ec0'0400U, 0x4ec0'0400U, 0x0ea0'c400U,
			0x4ea0'c400U, 0x4ee0'c400U}};
	constexpr std::array minmax_scalar_pair{
		std::array{0x5e30'f800U, 0x7e30'f800U, 0x7e70'f800U},
		std::array{0x5e30'c800U, 0x7e30'c800U, 0x7e70'c800U},
		std::array{0x5eb0'f800U, 0x7eb0'f800U, 0x7ef0'f800U},
		std::array{0x5eb0'c800U, 0x7eb0'c800U, 0x7ef0'c800U}};
	constexpr std::array minmax_vector_pair{
		std::array{0x2e40'3400U, 0x6e40'3400U, 0x2e20'f400U,
			0x6e20'f400U, 0x6e60'f400U},
		std::array{0x2e40'0400U, 0x6e40'0400U, 0x2e20'c400U,
			0x6e20'c400U, 0x6e60'c400U},
		std::array{0x2ec0'3400U, 0x6ec0'3400U, 0x2ea0'f400U,
			0x6ea0'f400U, 0x6ee0'f400U},
		std::array{0x2ec0'0400U, 0x6ec0'0400U, 0x2ea0'c400U,
			0x6ea0'c400U, 0x6ee0'c400U}};
	constexpr std::array minmax_reduce{
		std::array{0x0e30'f800U, 0x4e30'f800U, 0x6e30'f800U},
		std::array{0x0e30'c800U, 0x4e30'c800U, 0x6e30'c800U},
		std::array{0x0eb0'f800U, 0x4eb0'f800U, 0x6eb0'f800U},
		std::array{0x0eb0'c800U, 0x4eb0'c800U, 0x6eb0'c800U}};

	std::uint32_t value{};
	if (form.operation == Operation::absolute_difference)
		value = form.kind == Kind::scalar_binary ?
			fabd_scalar[scalar_shape(form.width)] :
			fabd_vector[vector_shape(form.width, form.vector_bytes)];
	else if (form.operation == Operation::add)
		value = form.kind == Kind::scalar_pairwise ?
			faddp_scalar[scalar_shape(form.width)] :
			faddp_vector[vector_shape(form.width, form.vector_bytes)];
	else {
		const unsigned operation = operation_index(form.operation);
		if (form.kind == Kind::scalar_binary)
			value = minmax_scalar_binary[operation][scalar_shape(form.width)];
		else if (form.kind == Kind::vector_binary)
			value = minmax_vector_binary[operation][vector_shape(
				form.width, form.vector_bytes)];
		else if (form.kind == Kind::scalar_pairwise)
			value = minmax_scalar_pair[operation][scalar_shape(form.width)];
		else if (form.kind == Kind::vector_pairwise)
			value = minmax_vector_pair[operation][vector_shape(
				form.width, form.vector_bytes)];
		else
			value = minmax_reduce[operation][form.width == 32U ? 2U :
				form.vector_bytes == 16U ? 1U : 0U];
	}
	value |= registers.destination & 31U;
	value |= (registers.first & 31U) << 5;
	if (form.kind == Kind::scalar_binary ||
	    form.kind == Kind::vector_binary ||
	    form.kind == Kind::vector_pairwise)
		value |= (registers.second & 31U) << 16;
	return value;
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(const Form &form)
{
	using uml::backend::ArmFeature;
	if (form.operation == Operation::absolute_maximum ||
	    form.operation == Operation::absolute_minimum)
		return engine_tests::NativeRequirements::features(
			{ArmFeature::advanced_simd, ArmFeature::faminmax});
	const bool scalar_fp = form.kind == Kind::scalar_binary &&
		form.operation != Operation::absolute_difference;
	auto requirements = engine_tests::NativeRequirements::features(
		scalar_fp ? ArmFeature::floating_point :
			ArmFeature::advanced_simd);
	return form.width == 16U ?
		requirements | ArmFeature::fp16 : requirements;
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements_for_state(const Form &form, std::uint32_t fpcr)
{
	auto result = native_requirements(form);
	const bool absolute =
		form.operation == Operation::absolute_maximum ||
		form.operation == Operation::absolute_minimum;
	const bool scalar = form.kind == Kind::scalar_binary;
	const bool ah_affected =
		form.operation == Operation::absolute_difference ||
		form.operation == Operation::add ||
		form.operation == Operation::maximum ||
		form.operation == Operation::minimum;
	const bool requires_afp =
		(scalar && (fpcr & 4U) != 0U) ||
		(!absolute && form.width != 16U &&
		 (fpcr & 1U) != 0U) ||
		(ah_affected && (fpcr & 2U) != 0U);
	if (requires_afp)
		result = result | uml::backend::ArmFeature::afp;
	return result;
}

struct ExpectedCounts final {
	std::size_t cases{};
	std::size_t jit{};
	std::size_t feature_skips{};

	void add(engine_tests::NativeRequirements requirements,
		bool invalid_encoding = false)
	{
		++cases;
		if (invalid_encoding || engine_tests::host_supports(requirements))
			++jit;
		else
			++feature_skips;
	}
};

inline void require_counts(const engine_tests::EngineHarness &harness,
	const ExpectedCounts &expected)
{
	const auto &actual = harness.counts();
	common::require(actual.interpreter == expected.cases);
	common::require(actual.gadgets == expected.cases);
	common::require(actual.jit == expected.jit);
	common::require(actual.jit_skipped == expected.feature_skips);
	common::require(actual.jit_skipped_host_feature ==
		expected.feature_skips);
	common::require(actual.jit_unavailable == 0U);
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0)
{
	return basic::state(seed);
}

[[nodiscard]] constexpr unsigned source_lanes(const Form &form) noexcept
{
	if (form.kind == Kind::scalar_binary)
		return 1U;
	if (form.kind == Kind::scalar_pairwise)
		return 2U;
	return form.vector_bytes / (form.width / 8U);
}

[[nodiscard]] constexpr unsigned result_lanes(const Form &form) noexcept
{
	return form.kind == Kind::scalar_binary ||
		form.kind == Kind::scalar_pairwise ||
		form.kind == Kind::reduction ? 1U :
		form.vector_bytes / (form.width / 8U);
}

inline void set_lane_bits(arm64::CpuState &cpu, unsigned reg,
	unsigned lane, unsigned width, std::uint64_t bits)
{
	basic::set_lane_bits(cpu, reg, lane, width, bits);
}

[[nodiscard]] inline std::uint64_t lane_bits(const arm64::CpuState &cpu,
	unsigned reg, unsigned lane, unsigned width)
{
	return basic::lane_bits_raw(cpu, reg, lane, width);
}

inline void fill_lanes(arm64::CpuState &cpu, unsigned reg,
	unsigned lanes, unsigned width, std::uint64_t bits)
{
	for (unsigned lane = 0; lane < lanes; ++lane)
		set_lane_bits(cpu, reg, lane, width, bits);
}

[[nodiscard]] inline arm64::CpuState expected_results(
	const arm64::CpuState &initial, const Form &form, Registers registers,
	std::span<const std::uint64_t> result_bits,
	std::uint32_t raised_flags = 0U)
{
	auto expected = initial;
	std::array<std::byte, 16> result{};
	if (form.kind == Kind::scalar_binary &&
	    (initial.fpcr & 4U) != 0U)
		result = initial.vectors[registers.first].bytes;
	const unsigned bytes = form.width / 8U;
	for (unsigned lane = 0; lane < result_lanes(form); ++lane)
		detail::encode_little_endian(result_bits[lane],
			std::span{result}.subspan(lane * bytes, bytes));
	expected.vectors[registers.destination].bytes = result;
	expected.fpsr |= raised_flags;
	return expected;
}

[[nodiscard]] inline arm64::CpuState expected_uniform(
	const arm64::CpuState &initial, const Form &form, Registers registers,
	std::uint64_t result_bits, std::uint32_t raised_flags = 0U)
{
	std::array<std::uint64_t, 8> results{};
	results.fill(result_bits);
	return expected_results(initial, form, registers,
		std::span{results}.first(result_lanes(form)), raised_flags);
}

inline void check_uniform(engine_tests::EngineHarness &harness,
	std::string_view name, const Form &form, Registers registers,
	const arm64::CpuState &initial, std::uint64_t result_bits,
	ExpectedCounts &counts, std::uint32_t raised_flags = 0U,
	engine_tests::NativeRequirements requirements =
		engine_tests::NativeRequirements::baseline())
{
	if (requirements.features().none())
		requirements = native_requirements_for_state(form, initial.fpcr);
	harness.check_one(name, instruction(form, registers), initial,
		expected_uniform(initial, form, registers, result_bits,
			raised_flags),
		requirements);
	counts.add(requirements);
}

template<typename Integer>
struct FloatLayout final {
	Integer sign;
	Integer fraction_mask;
	Integer exponent_mask;
	Integer quiet_bit;
	Integer magnitude_mask;
	Integer default_nan;
};

template<typename Integer>
[[nodiscard]] constexpr FloatLayout<Integer> float_layout() noexcept
{
	constexpr unsigned bits = sizeof(Integer) * 8U;
	constexpr unsigned fraction_bits = sizeof(Integer) == 2U ? 10U :
		sizeof(Integer) == 4U ? 23U : 52U;
	constexpr Integer sign = Integer{1} << (bits - 1U);
	constexpr Integer fraction_mask =
		static_cast<Integer>((Integer{1} << fraction_bits) - 1U);
	constexpr Integer exponent_mask = static_cast<Integer>(
		((Integer{1} << (bits - fraction_bits - 1U)) - 1U) <<
		fraction_bits);
	constexpr Integer quiet_bit = Integer{1} << (fraction_bits - 1U);
	return {sign, fraction_mask, exponent_mask, quiet_bit,
		static_cast<Integer>(sign - 1U),
		static_cast<Integer>(exponent_mask | quiet_bit)};
}

template<typename Integer>
[[nodiscard]] constexpr bool is_nan(Integer value) noexcept
{
	const auto layout = float_layout<Integer>();
	return (value & layout.exponent_mask) == layout.exponent_mask &&
		(value & layout.fraction_mask) != 0U;
}

template<typename Integer>
[[nodiscard]] constexpr bool is_signaling_nan(Integer value) noexcept
{
	const auto layout = float_layout<Integer>();
	return is_nan(value) && (value & layout.quiet_bit) == 0U;
}

template<typename Integer>
[[nodiscard]] constexpr bool is_zero(Integer value) noexcept
{
	return (value & float_layout<Integer>().magnitude_mask) == 0U;
}

template<typename Integer>
[[nodiscard]] constexpr bool is_subnormal(Integer value) noexcept
{
	const auto layout = float_layout<Integer>();
	return (value & layout.exponent_mask) == 0U &&
		(value & layout.fraction_mask) != 0U;
}

template<typename Integer>
[[nodiscard]] constexpr bool finite_less(Integer first, Integer second) noexcept
{
	const auto layout = float_layout<Integer>();
	if (is_zero(first) && is_zero(second))
		return false;
	const bool first_negative = (first & layout.sign) != 0U;
	const bool second_negative = (second & layout.sign) != 0U;
	if (first_negative != second_negative)
		return first_negative;
	const Integer first_magnitude = first & layout.magnitude_mask;
	const Integer second_magnitude = second & layout.magnitude_mask;
	return first_negative ? first_magnitude > second_magnitude :
		first_magnitude < second_magnitude;
}

template<typename Integer>
struct ElementResult final {
	Integer bits;
	std::uint32_t flags;
};

template<typename Integer>
[[nodiscard]] constexpr ElementResult<Integer> processed_nan(
	Integer first, Integer second, std::uint32_t fpcr,
	bool alternate_both_nan = false) noexcept
{
	const auto layout = float_layout<Integer>();
	const bool first_signaling = is_signaling_nan(first);
	const bool second_signaling = is_signaling_nan(second);
	Integer selected{};
	if (alternate_both_nan && is_nan(first) && is_nan(second))
		selected = first;
	else if (first_signaling)
		selected = first;
	else if (second_signaling)
		selected = second;
	else if (is_nan(first))
		selected = first;
	else
		selected = second;
	const bool invalid = first_signaling || second_signaling;
	if ((fpcr & (1U << 25)) != 0U)
		selected = layout.default_nan;
	else
		selected |= layout.quiet_bit;
	return {selected, invalid ? basic::fpsr_invalid : 0U};
}

template<typename Integer>
[[nodiscard]] constexpr ElementResult<Integer> minmax_element(
	Operation operation, Integer first, Integer second,
	std::uint32_t fpcr) noexcept
{
	const auto layout = float_layout<Integer>();
	const bool absolute = operation == Operation::absolute_maximum ||
		operation == Operation::absolute_minimum;
	const bool number = operation == Operation::maximum_number ||
		operation == Operation::minimum_number;
	const bool maximum = operation == Operation::maximum ||
		operation == Operation::maximum_number ||
		operation == Operation::absolute_maximum;
	if (absolute) {
		if (is_nan(first) || is_nan(second))
			return processed_nan(first, second, fpcr);
		const Integer first_magnitude = first & layout.magnitude_mask;
		const Integer second_magnitude = second & layout.magnitude_mask;
		return {maximum ?
			(first_magnitude > second_magnitude ?
				first_magnitude : second_magnitude) :
			(first_magnitude < second_magnitude ?
				first_magnitude : second_magnitude), 0U};
	}

	std::uint32_t flags{};
	const bool alternate = (fpcr & 2U) != 0U;
	const bool flush_input = (fpcr & 1U) != 0U;
	const bool flush_legacy = (fpcr & (1U << 24)) != 0U && !alternate;
	const bool flush_half = (fpcr & (1U << 19)) != 0U;
	const auto flush = [&](Integer value) {
		if (!is_subnormal(value))
			return value;
		if constexpr (sizeof(Integer) == 2U) {
			if (!flush_half)
				return value;
		} else {
			if (!flush_input && !flush_legacy)
				return value;
			if (flush_legacy)
				flags |= basic::fpsr_input_denormal;
		}
		return static_cast<Integer>(value & layout.sign);
	};
	first = flush(first);
	second = flush(second);

	if (!number && alternate &&
	    (is_nan(first) || is_nan(second))) {
		return {is_zero(second) ?
			static_cast<Integer>(second & layout.sign) : second,
			static_cast<std::uint32_t>(
				flags | basic::fpsr_invalid)};
	}

	if (number) {
		const bool first_qnan = is_nan(first) &&
			!is_signaling_nan(first);
		const bool second_qnan = is_nan(second) &&
			!is_signaling_nan(second);
		if (!(alternate && is_nan(first) && is_nan(second))) {
			if (first_qnan && !second_qnan)
				first = maximum ? layout.sign | layout.exponent_mask :
					layout.exponent_mask;
			else if (!first_qnan && second_qnan)
				second = maximum ? layout.sign | layout.exponent_mask :
					layout.exponent_mask;
		}
	}

	if (is_nan(first) || is_nan(second)) {
		auto result = processed_nan(first, second, fpcr,
			number && alternate && is_nan(first) && is_nan(second));
		result.flags |= flags;
		return result;
	}

	if (!number && alternate && is_zero(first) && is_zero(second))
		return {second, flags};
	if (alternate && sizeof(Integer) != 2U &&
	    (is_subnormal(first) || is_subnormal(second)))
		flags |= basic::fpsr_input_denormal;
	if (is_zero(first) && is_zero(second))
		return {maximum ?
			static_cast<Integer>(first & second & layout.sign) :
			static_cast<Integer>((first | second) & layout.sign), flags};
	const bool choose_first = maximum ?
		finite_less(second, first) : finite_less(first, second);
	Integer selected = choose_first ? first : second;
	if (number && alternate && is_subnormal(selected)) {
		const bool flush_output = sizeof(Integer) == 2U ?
			(fpcr & (1U << 19)) != 0U :
			(fpcr & (1U << 24)) != 0U;
		if (flush_output) {
			selected &= layout.sign;
			flags |= basic::fpsr_underflow | basic::fpsr_inexact;
		}
	}
	return {selected, flags};
}

template<typename Integer>
[[nodiscard]] arm64::CpuState expected_minmax(
	const arm64::CpuState &initial, const Form &form, Registers registers)
{
	std::array<std::uint64_t, 8> results{};
	std::uint32_t flags{};
	const unsigned lanes = source_lanes(form);
	const auto apply = [&](Integer first, Integer second) {
		const auto element = minmax_element(form.operation,
			first, second, initial.fpcr);
		flags |= element.flags;
		return element.bits;
	};
	if (form.kind == Kind::scalar_binary) {
		results[0] = apply(static_cast<Integer>(lane_bits(initial,
			registers.first, 0, form.width)),
			static_cast<Integer>(lane_bits(initial,
				registers.second, 0, form.width)));
	} else if (form.kind == Kind::scalar_pairwise) {
		results[0] = apply(static_cast<Integer>(lane_bits(initial,
			registers.first, 0, form.width)),
			static_cast<Integer>(lane_bits(initial,
				registers.first, 1, form.width)));
	} else if (form.kind == Kind::vector_binary) {
		for (unsigned lane = 0; lane < lanes; ++lane)
			results[lane] = apply(static_cast<Integer>(lane_bits(initial,
				registers.first, lane, form.width)),
				static_cast<Integer>(lane_bits(initial,
					registers.second, lane, form.width)));
	} else if (form.kind == Kind::vector_pairwise) {
		for (unsigned lane = 0; lane < lanes; ++lane) {
			const unsigned source = lane < lanes / 2U ?
				registers.first : registers.second;
			const unsigned pair = (lane % (lanes / 2U)) * 2U;
			results[lane] = apply(static_cast<Integer>(lane_bits(initial,
				source, pair, form.width)),
				static_cast<Integer>(lane_bits(initial,
					source, pair + 1U, form.width)));
		}
	} else {
		std::array<Integer, 8> values{};
		for (unsigned lane = 0; lane < lanes; ++lane)
			values[lane] = static_cast<Integer>(lane_bits(initial,
				registers.first, lane, form.width));
		for (unsigned remaining = lanes; remaining > 1U;
		     remaining /= 2U)
			for (unsigned lane = 0; lane < remaining / 2U; ++lane)
				values[lane] = apply(values[lane * 2U],
					values[lane * 2U + 1U]);
		results[0] = values[0];
	}
	return expected_results(initial, form, registers,
		std::span{results}.first(result_lanes(form)), flags);
}

inline void check_minmax(engine_tests::EngineHarness &harness,
	std::string_view name, const Form &form, Registers registers,
	const arm64::CpuState &initial, ExpectedCounts &counts,
	engine_tests::NativeRequirements native =
		engine_tests::NativeRequirements::baseline())
{
	if (native.features().none())
		native = native_requirements_for_state(form, initial.fpcr);
	arm64::CpuState expected;
	if (form.width == 16U)
		expected = expected_minmax<std::uint16_t>(
			initial, form, registers);
	else if (form.width == 32U)
		expected = expected_minmax<std::uint32_t>(
			initial, form, registers);
	else
		expected = expected_minmax<std::uint64_t>(
			initial, form, registers);
	harness.check_one(name, instruction(form, registers), initial,
		expected, native);
	counts.add(native);
}

} // namespace arm64_floating_horizontal_test
