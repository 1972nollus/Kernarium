// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_basic_floating_test_support.hpp"

#include <array>

namespace test = arm64_basic_floating_test;

namespace {

struct OperationValues final {
	std::uint64_t left;
	std::uint64_t right;
	std::uint64_t result;
	std::uint32_t flags;
};

[[nodiscard]] test::engine_tests::NativeRequirements merging_requirements(
	const test::Form &form)
{
	return test::native_requirements(form) | uml::backend::ArmFeature::afp;
}

void scalar_merging_uses_the_architectural_source()
{
	test::engine_tests::EngineHarness harness;
	constexpr std::array registers{
		test::Registers{0, 1, 2},
		test::Registers{1, 1, 2},
		test::Registers{2, 1, 2},
		test::Registers{1, 1, 1},
	};
	unsigned seed = 2000;
	for (const auto operation : test::scalar_operations)
		for (const auto width : test::widths)
			for (const auto register_case : registers) {
				const test::Form form{operation, true, width, 16U};
				auto initial = test::state(++seed);
				initial.fpcr = 4U;
				initial.fpsr = 1U << 27;
				test::prepare_exact(initial, form, register_case);
				const auto expected =
					test::expected_exact(initial, form, register_case);
				const auto &merge_source =
					operation == test::Operation::square_root ?
						initial.vectors[
							register_case.destination].bytes :
						initial.vectors[register_case.left].bytes;
				for (unsigned byte = width / 8U; byte < 16; ++byte)
					test::common::require(
						expected.vectors[
							register_case.destination].bytes[byte] ==
						merge_source[byte]);
				harness.check_one("basic-floating-scalar-merging",
					test::instruction(form, register_case), initial,
					expected, merging_requirements(form));
			}
	harness.print_summary("arm64-basic-floating-merging");
}

[[nodiscard]] OperationValues rounding_values(
	const test::FormatBits &format, test::Operation operation,
	unsigned rounding_mode)
{
	const bool toward_plus = rounding_mode == 1U;
	const bool toward_minus = rounding_mode == 2U;
	switch (operation) {
	case test::Operation::multiply:
		return {format.one_next, format.one_next,
			format.one + (toward_plus ? 3U : 2U),
			test::fpsr_inexact};
	case test::Operation::divide:
		return {format.one, format.one_next,
			format.one - (toward_plus ? 1U : 2U),
			test::fpsr_inexact};
	case test::Operation::add:
		return {format.one, format.half_ulp,
			format.one + (toward_plus ? 1U : 0U),
			test::fpsr_inexact};
	case test::Operation::subtract:
		return {format.one, format.quarter_ulp,
			toward_minus || rounding_mode == 3U ?
				format.one_previous : format.one,
			test::fpsr_inexact};
	case test::Operation::negative_multiply: {
		const auto product = format.one + (toward_plus ? 3U : 2U);
		return {format.one_next, format.one_next,
			test::negate_bits(format, product), test::fpsr_inexact};
	}
	case test::Operation::square_root:
		return {format.one_next, 0,
			format.one + (toward_plus ? 1U : 0U),
			test::fpsr_inexact};
	}
	__builtin_unreachable();
}

void every_rounding_mode_for_every_form()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 3000;
	test::for_each_form([&](const test::Form &form) {
		constexpr test::Registers registers{0, 1, 2};
		const auto &format = test::format_bits(form.width);
		for (unsigned rounding_mode = 0; rounding_mode < 4;
		     ++rounding_mode) {
			const auto values =
				rounding_values(format, form.operation, rounding_mode);
			auto initial = test::state(++seed);
			initial.fpcr = rounding_mode << 22;
			initial.fpsr = 1U << 27;
			test::prepare_uniform_inputs(initial, form, registers,
				values.left, values.right);
			harness.check_one("basic-floating-rounding-mode",
				test::instruction(form, registers), initial,
				test::expected_with_uniform_result(initial, form,
					registers, values.result, values.flags),
				test::native_requirements(form));
		}
	});
	harness.print_summary("arm64-basic-floating-rounding");
}

[[nodiscard]] OperationValues invalid_values(
	const test::FormatBits &format, test::Operation operation)
{
	switch (operation) {
	case test::Operation::multiply:
		return {format.positive_infinity, format.positive_zero,
			format.default_nan, test::fpsr_invalid};
	case test::Operation::divide:
		return {format.positive_zero, format.positive_zero,
			format.default_nan, test::fpsr_invalid};
	case test::Operation::add:
		return {format.positive_infinity,
			test::negate_bits(format, format.positive_infinity),
			format.default_nan, test::fpsr_invalid};
	case test::Operation::subtract:
		return {format.positive_infinity, format.positive_infinity,
			format.default_nan, test::fpsr_invalid};
	case test::Operation::negative_multiply:
		return {format.positive_infinity, format.positive_zero,
			test::negate_bits(format, format.default_nan),
			test::fpsr_invalid};
	case test::Operation::square_root:
		return {test::negate_bits(format, format.one), 0,
			format.default_nan, test::fpsr_invalid};
	}
	__builtin_unreachable();
}

[[nodiscard]] std::uint64_t nan_result(const test::FormatBits &format,
	test::Operation operation, std::uint64_t input)
{
	const auto quieted = input == format.signaling_nan ?
		format.quiet_nan : input;
	return operation == test::Operation::negative_multiply ?
		test::negate_bits(format, quieted) : quieted;
}

void every_special_value_and_status_accumulation()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 4000;
	test::for_each_form([&](const test::Form &form) {
		constexpr test::Registers registers{0, 1, 2};
		const auto &format = test::format_bits(form.width);

		{
			const auto values =
				invalid_values(format, form.operation);
			auto initial = test::state(++seed);
			initial.fpsr = 1U << 27;
			test::prepare_uniform_inputs(initial, form, registers,
				values.left, values.right);
			harness.check_one("basic-floating-invalid-operation",
				test::instruction(form, registers), initial,
				test::expected_with_uniform_result(initial, form,
					registers, values.result, values.flags),
				test::native_requirements(form));
		}

		for (const auto input :
		     {format.quiet_nan, format.signaling_nan}) {
			auto initial = test::state(++seed);
			initial.fpsr = 1U << 27;
			test::prepare_uniform_inputs(initial, form, registers,
				input, format.one);
			harness.check_one("basic-floating-nan-propagation",
				test::instruction(form, registers), initial,
				test::expected_with_uniform_result(initial, form,
					registers,
					nan_result(format, form.operation, input),
					input == format.signaling_nan ?
						test::fpsr_invalid : 0U),
				test::native_requirements(form));
		}

		{
			auto initial = test::state(++seed);
			initial.fpcr = 1U << 25;
			initial.fpsr = 1U << 27;
			test::prepare_uniform_inputs(initial, form, registers,
				format.quiet_nan, format.one);
			const auto result =
				form.operation ==
						test::Operation::negative_multiply ?
					test::negate_bits(format,
						format.default_nan) :
					format.default_nan;
			harness.check_one("basic-floating-default-nan",
				test::instruction(form, registers), initial,
				test::expected_with_uniform_result(initial, form,
					registers, result),
				test::native_requirements(form));
		}
	});
	harness.print_summary("arm64-basic-floating-special");
}

[[nodiscard]] OperationValues zero_values(
	const test::FormatBits &format, test::Operation operation)
{
	switch (operation) {
	case test::Operation::multiply:
		return {format.negative_zero, format.two, format.negative_zero, 0};
	case test::Operation::divide:
		return {format.negative_zero, format.two, format.negative_zero, 0};
	case test::Operation::add:
		return {format.positive_zero, format.negative_zero,
			format.positive_zero, 0};
	case test::Operation::subtract:
		return {format.positive_zero, format.positive_zero,
			format.positive_zero, 0};
	case test::Operation::negative_multiply:
		return {format.negative_zero, format.two, format.positive_zero, 0};
	case test::Operation::square_root:
		return {format.negative_zero, 0, format.negative_zero, 0};
	}
	__builtin_unreachable();
}

void signed_zero_and_divide_by_zero()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 5000;
	test::for_each_form([&](const test::Form &form) {
		constexpr test::Registers registers{0, 1, 2};
		const auto &format = test::format_bits(form.width);
		const auto values = zero_values(format, form.operation);
		auto initial = test::state(++seed);
		initial.fpsr = 1U << 27;
		test::prepare_uniform_inputs(initial, form, registers,
			values.left, values.right);
		harness.check_one("basic-floating-signed-zero",
			test::instruction(form, registers), initial,
			test::expected_with_uniform_result(initial, form, registers,
				values.result), test::native_requirements(form));

		if (form.operation != test::Operation::divide)
			return;
		initial = test::state(++seed);
		initial.fpsr = 1U << 27;
		test::prepare_uniform_inputs(initial, form, registers,
			format.one, format.positive_zero);
		harness.check_one("basic-floating-divide-by-zero",
			test::instruction(form, registers), initial,
			test::expected_with_uniform_result(initial, form, registers,
				format.positive_infinity,
				test::fpsr_divide_by_zero),
			test::native_requirements(form));
	});
	harness.print_summary("arm64-basic-floating-zero");
}

[[nodiscard]] OperationValues overflow_values(
	const test::FormatBits &format, test::Operation operation,
	unsigned rounding_mode)
{
	const bool infinity = rounding_mode == 0U || rounding_mode == 1U;
	const auto positive_result =
		infinity ? format.positive_infinity : format.maximum_finite;
	switch (operation) {
	case test::Operation::multiply:
		return {format.maximum_finite, format.two, positive_result,
			test::fpsr_overflow | test::fpsr_inexact};
	case test::Operation::divide:
		return {format.maximum_finite, format.half, positive_result,
			test::fpsr_overflow | test::fpsr_inexact};
	case test::Operation::add:
		return {format.maximum_finite, format.maximum_finite,
			positive_result,
			test::fpsr_overflow | test::fpsr_inexact};
	case test::Operation::subtract:
		return {format.maximum_finite,
			test::negate_bits(format, format.maximum_finite),
			positive_result,
			test::fpsr_overflow | test::fpsr_inexact};
	case test::Operation::negative_multiply:
		return {format.maximum_finite, format.two,
			test::negate_bits(format, positive_result),
			test::fpsr_overflow | test::fpsr_inexact};
	case test::Operation::square_root:
		__builtin_unreachable();
	}
	__builtin_unreachable();
}

void overflow_and_underflow_rounding()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 6000;
	test::for_each_form([&](const test::Form &form) {
		if (form.operation == test::Operation::square_root)
			return;
		constexpr test::Registers registers{0, 1, 2};
		const auto &format = test::format_bits(form.width);
		for (unsigned rounding_mode = 0; rounding_mode < 4;
		     ++rounding_mode) {
			const auto values =
				overflow_values(format, form.operation, rounding_mode);
			auto initial = test::state(++seed);
			initial.fpcr = rounding_mode << 22;
			initial.fpsr = 1U << 27;
			test::prepare_uniform_inputs(initial, form, registers,
				values.left, values.right);
			harness.check_one("basic-floating-overflow",
				test::instruction(form, registers), initial,
				test::expected_with_uniform_result(initial, form,
					registers, values.result, values.flags),
				test::native_requirements(form));
		}

		if (form.operation != test::Operation::multiply &&
		    form.operation != test::Operation::divide &&
		    form.operation != test::Operation::negative_multiply)
			return;
		for (unsigned rounding_mode = 0; rounding_mode < 4;
		     ++rounding_mode) {
			const bool toward_plus = rounding_mode == 1U;
			auto initial = test::state(++seed);
			initial.fpcr = rounding_mode << 22;
			initial.fpsr = 1U << 27;
			test::prepare_uniform_inputs(initial, form, registers,
				format.minimum_subnormal,
				form.operation == test::Operation::divide ?
					format.two : format.half);
			auto result =
				toward_plus ? format.minimum_subnormal :
					format.positive_zero;
			if (form.operation ==
			    test::Operation::negative_multiply)
				result = test::negate_bits(format, result);
			harness.check_one("basic-floating-underflow",
				test::instruction(form, registers), initial,
				test::expected_with_uniform_result(initial, form,
					registers, result,
					test::fpsr_underflow |
						test::fpsr_inexact),
				test::native_requirements(form));
		}
	});
	harness.print_summary("arm64-basic-floating-range");
}

void flush_to_zero_control()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 7000;
	test::for_each_form([&](const test::Form &form) {
		if (form.operation != test::Operation::multiply &&
		    form.operation != test::Operation::divide &&
		    form.operation != test::Operation::negative_multiply)
			return;
		constexpr test::Registers registers{0, 1, 2};
		const auto &format = test::format_bits(form.width);
		auto initial = test::state(++seed);
		initial.fpcr = form.width == 16U ? 1U << 19 : 1U << 24;
		initial.fpsr = 1U << 27;
		test::prepare_uniform_inputs(initial, form, registers,
			format.minimum_normal,
			form.operation == test::Operation::divide ?
				format.two : format.half);
		auto result = format.positive_zero;
		if (form.operation == test::Operation::negative_multiply)
			result = format.negative_zero;
		harness.check_one("basic-floating-flush-to-zero",
			test::instruction(form, registers), initial,
			test::expected_with_uniform_result(initial, form, registers,
				result, test::fpsr_underflow),
			test::native_requirements(form));
	});
	harness.print_summary("arm64-basic-floating-flush");
}

} // namespace

int main()
{
	scalar_merging_uses_the_architectural_source();
	every_rounding_mode_for_every_form();
	every_special_value_and_status_accumulation();
	signed_zero_and_divide_by_zero();
	overflow_and_underflow_rounding();
	flush_to_zero_control();
	return 0;
}
