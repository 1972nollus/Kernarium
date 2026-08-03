// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fused_floating_test_support.hpp"

namespace test = arm64_fused_floating_test;

namespace {

template<typename Floating>
void scalar_nep_merges_from_addend(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts, unsigned width)
{
	constexpr test::Registers registers{0, 1, 2, 3};
	for (const bool negate : {false, true})
		for (const bool subtract : {false, true}) {
			const test::Form form{true, negate, subtract, width, 16U};
			auto initial = test::state(
				width + negate * 7U + subtract * 13U);
			initial.fpcr = 4U;
			initial.fpsr = 0;
			test::prepare_exact<Floating>(initial, form, registers);
			const auto result_bits =
				std::bit_cast<
					test::basic::FloatingInteger<Floating>>(
						test::exact_result<Floating>(form));
			const auto expected = test::expected_uniform(
				initial, form, registers, result_bits);
			const auto native = test::native_requirements(form) |
				::uml::cpu::ArmFeature::afp;
			harness.check_one(
				"scalar fused FPCR.NEP merges inactive bits from addend",
				test::instruction(form, registers), initial,
				expected, native);
			counts.add(native);
		}
}

void invalid_operation_uses_default_nan_and_sets_ioc(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	constexpr test::Registers registers{0, 1, 2, 3};
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		const auto &format = test::basic::format_bits(form.width);
		auto initial = test::state(200U + seed++);
		initial.fpcr = 1U << 25; // DN
		initial.fpsr = test::basic::fpsr_inexact;
		test::fill_active_lanes(initial, registers.left, form,
			format.positive_infinity);
		test::fill_active_lanes(initial, registers.right, form,
			format.positive_zero);
		test::fill_active_lanes(initial,
			form.scalar ? registers.addend :
				registers.destination,
			form, format.one);
		test::check_uniform(harness,
			"fused invalid zero-times-infinity default result",
			form, registers, initial, format.default_nan,
			test::basic::fpsr_invalid);
		counts.add(test::native_requirements(form));
	});
}

void quiet_nan_defaulting_does_not_set_ioc(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	constexpr test::Registers registers{0, 1, 2, 3};
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		const auto &format = test::basic::format_bits(form.width);
		auto initial = test::state(400U + seed++);
		initial.fpcr = 1U << 25; // DN
		initial.fpsr = test::basic::fpsr_underflow;
		test::fill_active_lanes(initial, registers.left, form,
			format.quiet_nan);
		test::fill_active_lanes(initial, registers.right, form,
			format.one);
		test::fill_active_lanes(initial,
			form.scalar ? registers.addend :
				registers.destination,
			form, format.one);
		test::check_uniform(harness,
			"fused quiet NaN defaulting preserves FPSR",
			form, registers, initial, format.default_nan);
		counts.add(test::native_requirements(form));
	});
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts expected;
	scalar_nep_merges_from_addend<_Float16>(
		harness, expected, 16U);
	scalar_nep_merges_from_addend<float>(
		harness, expected, 32U);
	scalar_nep_merges_from_addend<double>(
		harness, expected, 64U);
	invalid_operation_uses_default_nan_and_sets_ioc(harness, expected);
	quiet_nan_defaulting_does_not_set_ioc(harness, expected);
	test::require_counts(harness, expected);
	harness.print_summary("arm64-fused-floating-control");
}
