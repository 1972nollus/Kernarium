// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fused_floating_test_support.hpp"

namespace test = arm64_fused_floating_test;

namespace {

template<typename Floating>
void check_form(test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts, const test::Form &form,
	unsigned seed)
{
	constexpr test::Registers registers{0, 1, 2, 3};
	auto initial = test::state(seed);
	initial.fpcr = 0;
	initial.fpsr = 0;
	test::prepare_exact<Floating>(initial, form, registers);
	const auto expected_bits =
		std::bit_cast<test::basic::FloatingInteger<Floating>>(
		test::exact_result<Floating>(form));
	char name[128];
	std::snprintf(name, sizeof(name),
		"fused floating exact operand order form=%u",
		seed);
	test::check_uniform(harness, name, form, registers, initial,
		expected_bits);
	counts.add(test::native_requirements(form));
}

void exact_sign_and_accumulator_semantics(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.width == 16U)
			check_form<_Float16>(harness, counts, form, ++seed);
		else if (form.width == 32U)
			check_form<float>(harness, counts, form, ++seed);
		else
			check_form<double>(harness, counts, form, ++seed);
	});
	test::common::require(seed == 22U);
}

void single_rounding_is_genuinely_fused(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	constexpr test::Registers registers{0, 1, 2, 3};
	const test::Form scalar{true, false, false, 32U, 16U};
	auto initial = test::state(0);
	initial.fpcr = 0;
	initial.fpsr = 0;
	test::basic::set_lane(initial, 1, 0,
		std::bit_cast<float>(0x3f80'0001U));
	test::basic::set_lane(initial, 2, 0,
		std::bit_cast<float>(0x3f80'0001U));
	test::basic::set_lane(initial, 3, 0,
		std::bit_cast<float>(0xbf80'0002U));
	test::check_uniform(harness,
		"FPSIMD-0122 FMADD single-rounding witness",
		scalar, registers, initial, 0x2880'0000U);
	counts.add(test::native_requirements(scalar));
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts expected;
	exact_sign_and_accumulator_semantics(harness, expected);
	single_rounding_is_genuinely_fused(harness, expected);
	test::require_counts(harness, expected);
	harness.print_summary("arm64-fused-floating-behavior");
}
