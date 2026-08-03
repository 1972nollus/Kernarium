// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_reciprocal_test_support.hpp"

#include <array>

namespace test = arm64_floating_reciprocal_test;

namespace {

constexpr test::Registers registers{0, 1, 2};

[[nodiscard]] std::array<std::uint64_t, 12> unary_values(unsigned width)
{
	const auto value = test::format(width);
	const auto sign = test::sign_mask(value);
	const auto infinity = test::infinity_bits(value);
	return {
		std::uint64_t{0},
		sign,
		std::uint64_t{1},
		sign | 1U,
		test::fraction_mask(value),
		test::normal_bits(value, 1U, 0U),
		test::normal_bits(value,
			static_cast<unsigned>(value.bias + 2), 0x55U),
		infinity,
		sign | infinity,
		infinity |
			(std::uint64_t{1} << (value.fraction_bits - 1U)) |
			0x55U,
		infinity | 0x55U,
		sign | infinity | 0x55U,
	};
}

void scalar_merging_uses_the_specified_source(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 0;
	test::for_each_form([&](const test::Form &form) {
		if (!form.scalar)
			return;
		for (const auto register_set : {
			     test::Registers{0, 1, 2},
			     test::Registers{1, 1, 2},
			     test::Registers{2, 1, 2},
			     test::Registers{31, 30, 29},
		     }) {
			auto initial = test::state(++seed);
			initial.fpcr = test::fpcr_nep;
			test::prepare_simple(initial, form, register_set);
			test::check_one(harness, form, register_set, initial);
		}
	});
}

void standard_nan_zero_infinity_and_subnormal_rules(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 1000;
	test::for_each_form([&](const test::Form &form) {
		if (!test::unary(form.operation))
			return;
		const auto inputs = unary_values(form.width);
		const std::array controls{
			std::uint32_t{0},
			test::fpcr_dn,
			form.width == 16U ? test::fpcr_fz16 :
				test::fpcr_fz,
			form.width == 16U ? test::fpcr_fz16 | test::fpcr_dn :
				test::fpcr_fiz,
		};
		for (const auto control : controls)
			for (unsigned input = 0; input < inputs.size(); ++input) {
				auto initial = test::state(++seed);
				initial.fpcr = control;
				for (unsigned lane = 0;
				     lane < test::active_lanes(form); ++lane)
					test::write_lane(
						initial.vectors[registers.first].bytes,
						lane, form.width,
						inputs[(input + lane) %
							inputs.size()]);
				test::check_one(
					harness, form, registers, initial);
			}
	});
}

void alternate_behavior_special_cases(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 5000;
	test::for_each_form([&](const test::Form &form) {
		const auto value = test::format(form.width);
		const auto infinity = test::infinity_bits(value);
		const auto quiet = infinity |
			(std::uint64_t{1} << (value.fraction_bits - 1U)) |
			0x55U;
		const auto signaling = infinity | 0x33U;
		const auto subnormal = std::uint64_t{1};
		const std::array controls{
			test::fpcr_ah,
			test::fpcr_ah | test::fpcr_dn,
			test::fpcr_ah | test::fpcr_nep,
			test::fpcr_ah |
				(form.width == 16U ? test::fpcr_fz16 :
					test::fpcr_fz),
		};
		for (const auto control : controls) {
			auto check = [&](std::uint64_t first,
					 std::uint64_t second) {
				auto initial = test::state(++seed);
				initial.fpcr = control;
				test::fill_lanes(initial, registers.first, form, first);
				if (!test::unary(form.operation))
					test::fill_lanes(initial, registers.second,
						form, second);
				test::check_one(
					harness, form, registers, initial);
			};
			check(0U, infinity);
			check(test::sign_mask(value), infinity);
			check(infinity, 0U);
			check(quiet, signaling);
			check(signaling, quiet);
			check(subnormal, infinity);
			if (form.operation ==
			    test::Operation::reciprocal_sqrt_estimate)
				check(test::normal_bits(value,
					static_cast<unsigned>(value.bias),
					0U, true), 0U);
			if (!test::unary(form.operation))
				check(test::normal_bits(value,
					static_cast<unsigned>(value.bias),
					test::fraction_mask(value) / 3U),
					test::normal_bits(value,
						static_cast<unsigned>(value.bias - 1),
						test::fraction_mask(value) / 5U));
		}
	});
}

void reciprocal_overflow_obeys_rounding_mode(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 8000;
	test::for_each_form([&](const test::Form &form) {
		if (form.operation != test::Operation::reciprocal_estimate)
			return;
		const auto value = test::format(form.width);
		const auto tiny =
			std::uint64_t{1} <<
			(value.fraction_bits > 3U ? value.fraction_bits - 3U :
				0U);
		for (unsigned rounding = 0; rounding < 4U; ++rounding)
			for (const bool negative : {false, true}) {
				auto initial = test::state(++seed);
				initial.fpcr = rounding << 22U;
				test::fill_lanes(initial, registers.first, form,
					test::zero_bits(value, negative) | tiny);
				test::check_one(
					harness, form, registers, initial);
			}
	});
}

[[nodiscard]] bool requires_afp(
	test::engine_tests::NativeRequirements requirements)
{
	return requirements.features().test(static_cast<std::size_t>(
		::uml::cpu::ArmFeature::afp));
}

void native_requirement_relevance()
{
	const test::Form half_vector{
		test::Operation::reciprocal_estimate, false, 16U, 8U};
	const test::Form single_vector{
		test::Operation::reciprocal_estimate, false, 32U, 8U};
	const test::Form half_scalar{
		test::Operation::reciprocal_estimate, true, 16U, 16U};
	test::require(!requires_afp(
			test::native_requirements(half_vector, test::fpcr_fiz)),
		"binary16 FIZ incorrectly requires AFP");
	test::require(requires_afp(
			test::native_requirements(single_vector, test::fpcr_fiz)),
		"binary32 FIZ failed to require AFP");
	test::require(!requires_afp(
			test::native_requirements(half_vector, test::fpcr_nep)),
		"vector NEP incorrectly requires AFP");
	test::require(requires_afp(
			test::native_requirements(half_scalar, test::fpcr_nep)),
		"scalar NEP failed to require AFP");
	test::require(requires_afp(
			test::native_requirements(half_vector, test::fpcr_ah)),
		"AH failed to require AFP");
}

[[nodiscard]] test::ExpectedEngineCounts expected_counts()
{
	test::ExpectedEngineCounts expected;
	test::for_each_form([&](const test::Form &form) {
		const std::array alternate_controls{
			test::fpcr_ah,
			test::fpcr_ah | test::fpcr_dn,
			test::fpcr_ah | test::fpcr_nep,
			test::fpcr_ah |
				(form.width == 16U ? test::fpcr_fz16 :
					test::fpcr_fz),
		};
		const std::size_t alternate_cases =
			6U +
			(form.operation ==
				test::Operation::reciprocal_sqrt_estimate ? 1U : 0U) +
			(!test::unary(form.operation) ? 1U : 0U);
		for (const auto control : alternate_controls)
			expected.add(form, control, alternate_cases);

		if (test::unary(form.operation)) {
			const std::array standard_controls{
				std::uint32_t{0},
				test::fpcr_dn,
				form.width == 16U ? test::fpcr_fz16 :
					test::fpcr_fz,
				form.width == 16U ?
					test::fpcr_fz16 | test::fpcr_dn :
					test::fpcr_fiz,
			};
			for (const auto control : standard_controls)
				expected.add(form, control, 12U);
		}
		if (form.operation == test::Operation::reciprocal_estimate)
			for (unsigned rounding = 0; rounding < 4U; ++rounding)
				expected.add(form, rounding << 22U, 2U);
		if (form.scalar)
			expected.add(form, test::fpcr_nep, 4U);
	});
	test::require(expected.total == 1'972U,
		"control formula omitted an FPCR behavior case");
	return expected;
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	native_requirement_relevance();
	alternate_behavior_special_cases(harness);
	standard_nan_zero_infinity_and_subnormal_rules(harness);
	reciprocal_overflow_obeys_rounding_mode(harness);
	scalar_merging_uses_the_specified_source(harness);
	test::require_engine_counts(harness, expected_counts());
	harness.print_summary("arm64-floating-reciprocal-control");
	return 0;
}
