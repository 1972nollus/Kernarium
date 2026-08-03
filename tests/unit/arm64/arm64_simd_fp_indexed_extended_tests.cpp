// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fp_indexed_test_support.hpp"

#include <array>

namespace test = arm64_simd_fp_indexed_test;

namespace {

struct FormatBits final {
	unsigned lane_bytes;
	std::uint64_t sign;
	std::uint64_t zero;
	std::uint64_t half;
	std::uint64_t one;
	std::uint64_t two;
	std::uint64_t infinity;
	std::uint64_t maximum;
	std::uint64_t minimum_normal;
	std::uint64_t minimum_subnormal;
	std::uint64_t subnormal_to_normal_scale;
	std::uint64_t quiet_nan;
	std::uint64_t signaling_nan;
	std::uint64_t default_nan;
	std::uint64_t quiet_bit;
};

inline constexpr std::array formats{
	FormatBits{2U, 0x8000U, 0U, 0x3800U, 0x3c00U, 0x4000U,
		0x7c00U, 0x7bffU, 0x0400U, 1U, 0x6400U,
		0x7e55U, 0x7c55U,
		0x7e00U, 0x0200U},
	FormatBits{4U, 0x8000'0000U, 0U, 0x3f00'0000U, 0x3f80'0000U,
		0x4000'0000U, 0x7f80'0000U, 0x7f7f'ffffU,
		0x0080'0000U, 1U, 0x4b00'0000U,
		0x7fc0'0055U, 0x7f80'0055U,
		0x7fc0'0000U, 0x0040'0000U},
	FormatBits{8U, 0x8000'0000'0000'0000ULL, 0U,
		0x3fe0'0000'0000'0000ULL, 0x3ff0'0000'0000'0000ULL,
		0x4000'0000'0000'0000ULL, 0x7ff0'0000'0000'0000ULL,
		0x7fef'ffff'ffff'ffffULL, 0x0010'0000'0000'0000ULL,
		1U, 0x4330'0000'0000'0000ULL,
		0x7ff8'0000'0000'0055ULL,
		0x7ff0'0000'0000'0055ULL, 0x7ff8'0000'0000'0000ULL,
		0x0008'0000'0000'0000ULL},
};

[[nodiscard]] constexpr const FormatBits &format(const test::Shape &shape)
{
	return shape.lane_bytes == 2U ? formats[0] :
		shape.lane_bytes == 4U ? formats[1] : formats[2];
}

[[nodiscard]] test::Registers registers(const test::Shape &shape)
{
	return {28U, 29U, shape.size == 0U ? 13U : 19U};
}

void prepare_uniform(test::arm64::CpuState &initial,
	const test::Shape &shape, test::Registers regs, unsigned index,
	std::uint64_t accumulator, std::uint64_t left,
	std::uint64_t selected)
{
	const auto &bits = format(shape);
	test::fill_bits(initial.vectors[regs.destination].bytes,
		shape.lane_bytes, accumulator);
	test::fill_bits(initial.vectors[regs.left].bytes,
		shape.lane_bytes, left);
	test::fill_bits(initial.vectors[regs.element].bytes,
		shape.lane_bytes, bits.one);
	test::write_bits(initial.vectors[regs.element].bytes, shape.lane_bytes,
		index, selected);
}

[[nodiscard]] std::uint64_t signed_bits(std::uint64_t bits,
	const FormatBits &format_bits, bool negative)
{
	return negative ? bits | format_bits.sign : bits;
}

void zero_times_infinity_is_signed_two_without_invalid(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 60'000U;
	for (const auto shape : test::shapes)
		for (const bool negative_left : {false, true})
			for (const bool negative_element : {false, true}) {
				const auto &bits = format(shape);
				const auto regs = registers(shape);
				const auto index = shape.indexes - 1U;
				auto initial = test::state(++seed);
				initial.fpcr = 0U;
				initial.fpsr = test::fpsr_saturation;
				prepare_uniform(initial, shape, regs, index, bits.one,
					signed_bits(bits.zero, bits, negative_left),
					signed_bits(bits.infinity, bits,
						negative_element));
				test::check_uniform(harness,
					"fp-indexed-fmulx-zero-infinity",
					test::Operation::fmulx, shape, regs, index,
					initial,
					signed_bits(bits.two, bits,
						negative_left != negative_element));
			}
}

void scalar_merging_uses_the_architectural_source(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 70'000U;
	for (const auto operation : test::operations)
		for (const auto shape :
		     {test::shapes[0], test::shapes[3], test::shapes[6]}) {
			const auto &bits = format(shape);
			const auto regs = registers(shape);
			const auto index = shape.indexes - 1U;
			auto initial = test::state(++seed);
			initial.fpcr = 4U;
			initial.fpsr = test::fpsr_saturation;
			initial.pstate_sm = false;
			prepare_uniform(initial, shape, regs, index, bits.one,
				bits.two, test::floating_bits(shape.lane_bytes, 3.0));
			const auto result = operation == test::Operation::fmla ?
				test::floating_bits(shape.lane_bytes, 7.0) :
				operation == test::Operation::fmls ?
				test::floating_bits(shape.lane_bytes, -5.0) :
				test::floating_bits(shape.lane_bytes, 6.0);
			test::check_uniform(harness, "fp-indexed-scalar-merging",
				operation, shape, regs, index, initial, result);
		}
}

[[nodiscard]] std::uint64_t overflow_result(test::Operation operation,
	const FormatBits &bits, unsigned rounding_mode)
{
	const bool negative = operation == test::Operation::fmls;
	const bool infinity = negative ?
		rounding_mode == 0U || rounding_mode == 2U :
		rounding_mode == 0U || rounding_mode == 1U;
	return signed_bits(infinity ? bits.infinity : bits.maximum,
		bits, negative);
}

void every_rounding_mode_reports_overflow(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 80'000U;
	for (const auto operation : test::operations)
		for (const auto shape : test::shapes)
			for (unsigned rounding_mode = 0; rounding_mode < 4U;
			     ++rounding_mode) {
				const auto &bits = format(shape);
				const auto regs = registers(shape);
				const auto index = shape.indexes - 1U;
				auto initial = test::state(++seed);
				initial.fpcr = rounding_mode << 22;
				initial.fpsr = test::fpsr_saturation;
				prepare_uniform(initial, shape, regs, index, bits.zero,
					bits.maximum, bits.two);
				test::check_uniform(harness,
					"fp-indexed-overflow-rounding", operation,
					shape, regs, index, initial,
					overflow_result(operation, bits,
						rounding_mode),
					test::fpsr_overflow | test::fpsr_inexact);
			}
}

[[nodiscard]] std::uint64_t underflow_result(test::Operation operation,
	const FormatBits &bits, unsigned rounding_mode)
{
	if (operation == test::Operation::fmls)
		return signed_bits(rounding_mode == 2U ?
			bits.minimum_subnormal : bits.zero, bits, true);
	return rounding_mode == 1U ? bits.minimum_subnormal : bits.zero;
}

void every_rounding_mode_reports_underflow(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 90'000U;
	for (const auto operation : test::operations)
		for (const auto shape : test::shapes)
			for (unsigned rounding_mode = 0; rounding_mode < 4U;
			     ++rounding_mode) {
				const auto &bits = format(shape);
				const auto regs = registers(shape);
				const auto index = shape.indexes - 1U;
				auto initial = test::state(++seed);
				initial.fpcr = rounding_mode << 22;
				initial.fpsr = test::fpsr_saturation;
				prepare_uniform(initial, shape, regs, index, bits.zero,
					bits.minimum_subnormal, bits.half);
				test::check_uniform(harness,
					"fp-indexed-underflow-rounding", operation,
					shape, regs, index, initial,
					underflow_result(operation, bits,
						rounding_mode),
					test::fpsr_underflow | test::fpsr_inexact);
			}
}

void flush_to_zero_controls_inputs_and_outputs(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 100'000U;
	for (const auto operation : test::operations)
		for (const auto shape : test::shapes) {
			const auto &bits = format(shape);
			const auto regs = registers(shape);
			const auto index = shape.indexes - 1U;
			const auto flush = shape.lane_bytes == 2U ?
				1U << 19 : 1U << 24;

			auto output = test::state(++seed);
			output.fpcr = flush;
			output.fpsr = test::fpsr_saturation;
			prepare_uniform(output, shape, regs, index, bits.zero,
				bits.minimum_normal, bits.half);
			test::check_uniform(harness,
				"fp-indexed-flush-output", operation, shape, regs,
				index, output,
				signed_bits(bits.zero, bits,
					operation == test::Operation::fmls),
				test::fpsr_underflow);

			auto input = test::state(++seed);
			input.fpcr = flush;
			input.fpsr = test::fpsr_saturation;
			prepare_uniform(input, shape, regs, index, bits.zero,
				bits.minimum_subnormal, bits.two);
			test::check_uniform(harness,
				"fp-indexed-flush-input", operation, shape, regs,
				index, input, bits.zero,
				shape.lane_bytes == 2U ?
					0U : test::fpsr_input_denormal);
		}
}

void exact_denormal_inputs_preserve_their_value(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 110'000U;
	for (const auto operation : test::operations)
		for (const auto shape : test::shapes) {
			const auto &bits = format(shape);
			const auto regs = registers(shape);
			const auto index = shape.indexes - 1U;
			auto initial = test::state(++seed);
			initial.fpcr = 0U;
			initial.fpsr = test::fpsr_saturation;
			prepare_uniform(initial, shape, regs, index, bits.zero,
				bits.minimum_subnormal, bits.two);
			test::check_uniform(harness,
				"fp-indexed-exact-denormal-input", operation, shape,
				regs, index, initial,
				signed_bits(bits.minimum_subnormal * 2U, bits,
					operation == test::Operation::fmls));
		}
}

void nan_processing_and_invalid_operations_are_spec_exact(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 120'000U;
	for (const auto operation : test::operations)
		for (const auto shape : test::shapes) {
			const auto &bits = format(shape);
			const auto regs = registers(shape);
			const auto index = shape.indexes - 1U;

			auto quiet = test::state(++seed);
			quiet.fpcr = 0U;
			quiet.fpsr = test::fpsr_saturation;
			prepare_uniform(quiet, shape, regs, index, bits.one,
				bits.quiet_nan, bits.one);
			test::check_uniform(harness, "fp-indexed-quiet-nan",
				operation, shape, regs, index, quiet,
				signed_bits(bits.quiet_nan, bits,
					operation == test::Operation::fmls));

			auto signaling = test::state(++seed);
			signaling.fpcr = 0U;
			signaling.fpsr = test::fpsr_saturation;
			prepare_uniform(signaling, shape, regs, index, bits.one,
				bits.signaling_nan, bits.one);
			test::check_uniform(harness, "fp-indexed-signaling-nan",
				operation, shape, regs, index, signaling,
				signed_bits(bits.signaling_nan | bits.quiet_bit, bits,
					operation == test::Operation::fmls),
				test::fpsr_invalid);

			auto default_nan = test::state(++seed);
			default_nan.fpcr = 1U << 25;
			default_nan.fpsr = test::fpsr_saturation;
			prepare_uniform(default_nan, shape, regs, index, bits.one,
				bits.quiet_nan, bits.one);
			test::check_uniform(harness, "fp-indexed-default-nan",
				operation, shape, regs, index, default_nan,
				bits.default_nan);

			if (operation == test::Operation::fmulx)
				continue;
			auto invalid = test::state(++seed);
			invalid.fpcr = 0U;
			invalid.fpsr = test::fpsr_saturation;
			prepare_uniform(invalid, shape, regs, index, bits.one,
				bits.zero, bits.infinity);
			test::check_uniform(harness,
				"fp-indexed-invalid-operation", operation, shape,
				regs, index, invalid, bits.default_nan,
				test::fpsr_invalid);
		}
}

void alternative_nan_priority_matches_every_source_partition(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 130'000U;
	for (const auto operation : test::operations)
		for (const auto shape : test::shapes) {
			const auto &bits = format(shape);
			const auto regs = registers(shape);
			const auto index = shape.indexes - 1U;
			const auto addend_nan = bits.quiet_nan ^ 0x11U;
			const auto left_nan = bits.quiet_nan ^ 0x22U;
			const auto element_nan = bits.quiet_nan ^ 0x44U;
			const auto element_signaling = bits.signaling_nan ^ 0x88U;
			const auto addend_signaling = bits.signaling_nan ^ 0x110U;
			const auto left_signaling = bits.signaling_nan ^ 0x120U;
			const auto check = [&](std::string_view name,
				std::uint64_t addend, std::uint64_t left,
				std::uint64_t element, std::uint64_t expected,
				std::uint32_t flags = 0U) {
				auto initial = test::state(++seed);
				initial.fpcr = 2U;
				initial.fpsr = test::fpsr_saturation;
				prepare_uniform(initial, shape, regs, index,
					addend, left, element);
				test::check_uniform(harness, name, operation,
					shape, regs, index, initial, expected, flags);
			};
			if (test::accumulating(operation)) {
				check("fp-indexed-alternative-addend-nan",
					addend_nan, bits.one, bits.two, addend_nan);
				check("fp-indexed-alternative-left-nan",
					bits.one, left_nan, bits.two, left_nan);
				check("fp-indexed-alternative-element-nan",
					bits.one, bits.two, element_nan, element_nan);
				check("fp-indexed-alternative-addend-left",
					addend_nan, left_nan, bits.two, left_nan);
				check("fp-indexed-alternative-addend-element",
					addend_nan, bits.two, element_nan,
					element_nan);
				check("fp-indexed-alternative-left-element",
					bits.one, left_nan, element_signaling,
					left_nan | bits.quiet_bit,
					test::fpsr_invalid);
				check("fp-indexed-alternative-addend-signaling",
					addend_signaling, left_nan, bits.two,
					left_nan | bits.quiet_bit,
					test::fpsr_invalid);
				check("fp-indexed-alternative-all-nan",
					addend_nan, left_nan, element_signaling,
					left_nan | bits.quiet_bit,
					test::fpsr_invalid);
			} else {
				check("fp-indexed-alternative-left-nan",
					bits.one, left_nan, bits.two, left_nan);
				check("fp-indexed-alternative-element-nan",
					bits.one, bits.two, element_nan, element_nan);
				check("fp-indexed-alternative-both-nan",
					bits.one, left_nan, element_nan, left_nan);
				check("fp-indexed-alternative-right-signaling",
					bits.one, left_nan, element_signaling,
					left_nan | bits.quiet_bit,
					test::fpsr_invalid);
				check("fp-indexed-alternative-left-signaling",
					bits.one, left_signaling, element_nan,
					left_signaling | bits.quiet_bit,
					test::fpsr_invalid);
			}
		}
}

void alternative_default_nan_and_invalid_cases_are_spec_exact(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 140'000U;
	for (const auto operation : test::operations)
		for (const auto shape : test::shapes) {
			const auto &bits = format(shape);
			const auto regs = registers(shape);
			const auto index = shape.indexes - 1U;
			const auto negative_default = bits.default_nan | bits.sign;

			auto defaulted = test::state(++seed);
			defaulted.fpcr = 2U | (1U << 25);
			defaulted.fpsr = test::fpsr_saturation;
			prepare_uniform(defaulted, shape, regs, index, bits.one,
				bits.quiet_nan, bits.quiet_nan ^ 0x44U);
			test::check_uniform(harness,
				"fp-indexed-alternative-default-nan", operation,
				shape, regs, index, defaulted, negative_default);

			if (operation != test::Operation::fmulx) {
				auto invalid = test::state(++seed);
				invalid.fpcr = 2U;
				invalid.fpsr = test::fpsr_saturation;
				prepare_uniform(invalid, shape, regs, index, bits.zero,
					bits.zero, bits.infinity);
				test::check_uniform(harness,
					"fp-indexed-alternative-invalid", operation,
					shape, regs, index, invalid, negative_default,
					test::fpsr_invalid);
			}
			if (test::accumulating(operation)) {
				auto nan_suppresses_invalid = test::state(++seed);
				nan_suppresses_invalid.fpcr = 2U;
				nan_suppresses_invalid.fpsr = test::fpsr_saturation;
				prepare_uniform(nan_suppresses_invalid, shape, regs,
					index, bits.quiet_nan, bits.zero,
					bits.infinity);
				test::check_uniform(harness,
					"fp-indexed-alternative-nan-suppresses-invalid",
					operation, shape, regs, index,
					nan_suppresses_invalid, bits.quiet_nan);

			}
		}
}

void alternative_denormal_controls_are_spec_exact(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 150'000U;
	for (const auto operation : test::operations)
		for (const auto shape : test::shapes) {
			const auto &bits = format(shape);
			const auto regs = registers(shape);
			const auto index = shape.indexes - 1U;
			const auto flush = shape.lane_bytes == 2U ?
				1U << 19 : 1U << 24;
			auto output = test::state(++seed);
			output.fpcr = 2U | flush;
			output.fpsr = test::fpsr_saturation;
			prepare_uniform(output, shape, regs, index, bits.zero,
				bits.minimum_normal, bits.half);
			test::check_uniform(harness,
				"fp-indexed-alternative-flush-output", operation,
				shape, regs, index, output,
				signed_bits(bits.zero, bits,
					operation == test::Operation::fmls),
				test::fpsr_underflow | test::fpsr_inexact);

			auto input = test::state(++seed);
			input.fpcr = 2U | flush;
			input.fpsr = test::fpsr_saturation;
			prepare_uniform(input, shape, regs, index, bits.zero,
				bits.minimum_subnormal,
				bits.subnormal_to_normal_scale);
			const bool half = shape.lane_bytes == 2U;
			test::check_uniform(harness,
				"fp-indexed-alternative-flush-input", operation,
				shape, regs, index, input,
				half ? bits.zero :
					signed_bits(bits.minimum_normal, bits,
						operation == test::Operation::fmls));

			if (half) {
				auto ignored_input_flush = test::state(++seed);
				ignored_input_flush.fpcr = 1U;
				ignored_input_flush.fpsr = test::fpsr_saturation;
				prepare_uniform(ignored_input_flush, shape, regs, index,
					bits.zero, bits.minimum_subnormal,
					bits.subnormal_to_normal_scale);
				test::check_uniform(harness,
					"fp-indexed-half-input-flush-ignored",
					operation, shape, regs, index,
					ignored_input_flush,
					signed_bits(bits.minimum_normal, bits,
						operation == test::Operation::fmls));
				continue;
			}
			auto input_flush = test::state(++seed);
			input_flush.fpcr = 1U;
			input_flush.fpsr = test::fpsr_saturation;
			prepare_uniform(input_flush, shape, regs, index, bits.zero,
				bits.minimum_subnormal,
				bits.subnormal_to_normal_scale);
			test::check_uniform(harness,
				"fp-indexed-alternative-input-flush", operation,
				shape, regs, index, input_flush, bits.zero);
		}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	zero_times_infinity_is_signed_two_without_invalid(harness);
	scalar_merging_uses_the_architectural_source(harness);
	every_rounding_mode_reports_overflow(harness);
	every_rounding_mode_reports_underflow(harness);
	flush_to_zero_controls_inputs_and_outputs(harness);
	exact_denormal_inputs_preserve_their_value(harness);
	nan_processing_and_invalid_operations_are_spec_exact(harness);
	alternative_nan_priority_matches_every_source_partition(harness);
	alternative_default_nan_and_invalid_cases_are_spec_exact(harness);
	alternative_denormal_controls_are_spec_exact(harness);
	test::base::require(harness.counts().interpreter == 892U);
	test::base::require(harness.counts().gadgets == 892U);
	harness.print_summary("arm64-simd-fp-indexed-extended");
	return 0;
}
