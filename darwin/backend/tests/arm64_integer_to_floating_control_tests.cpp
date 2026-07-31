// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_integer_to_floating_test_support.hpp"

#include <array>

namespace test = arm64_integer_to_floating_test;

namespace {

[[nodiscard]] constexpr std::uint64_t positive_maximum(
	const test::Form &form) noexcept
{
	return form.unsigned_source ? test::low_mask(form.integer_width) :
		test::sign_mask(form.integer_width) - 1U;
}

void scalar_merging_and_vector_zeroing(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		const test::Registers registers{6U, 7U};
		auto initial = test::state(++seed);
		initial.fpcr = test::fpcr_nep;
		test::fill_source(initial, form, registers,
			positive_maximum(form) / 3U, true);
		harness.check_one(
			form.scalar ?
				"integer-to-floating-scalar-nep-merging" :
				"integer-to-floating-vector-nep-ignored",
			test::instruction(form, registers), initial,
			test::expected_state(initial, form, registers),
			form.scalar ?
				test::native_requirements(form) |
					uml::backend::ArmFeature::afp :
				test::native_requirements(form));
	}, false);
}

void alternative_half_format_is_ignored(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 1'000U;
	test::for_each_form([&](const test::Form &form) {
		if (form.destination_width != 16U)
			return;
		for (unsigned rounding = 0U; rounding < 4U; ++rounding) {
			const test::Registers registers{10U, 11U};
			auto initial = test::state(++seed);
			initial.fpcr = test::fpcr_ahp | (rounding << 22);
			test::fill_source(initial, form, registers,
				positive_maximum(form), false);
			test::check(harness,
				"integer-to-floating-ignores-alternative-half-format",
				form, registers, initial);
		}
	}, false);
}

void check_half_flush(test::engine_tests::EngineHarness &harness,
	const test::Form &form, std::uint64_t input,
	std::uint32_t fpcr, unsigned &seed)
{
	const test::Registers registers{14U, 15U};
	auto initial = test::state(++seed);
	initial.fpcr = fpcr;
	test::fill_source(initial, form, registers, input, false);
	auto native = test::native_requirements(form);
	if ((fpcr & test::fpcr_ah) != 0U)
		native = native | uml::backend::ArmFeature::afp;
	harness.check_one("integer-to-floating-half-output-flush",
		test::instruction(form, registers), initial,
		test::expected_state(initial, form, registers), native);
}

void half_output_flush_before_and_after_rounding(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 2'000U;
	for (const bool unsigned_source : {false, true}) {
		for (const unsigned integer_width : {32U, 64U}) {
			const test::Form form{
				test::Encoding::scalar_fixed, unsigned_source, true,
				integer_width, 16U, 2U, integer_width};
			const unsigned shift = integer_width - 24U;
			for (const auto input : std::array{
				     std::uint64_t{1} << shift,
				     (std::uint64_t{1} << shift) +
					     (std::uint64_t{1} << (shift - 1U)),
				     ((std::uint64_t{1} << 10U) - 1U) << shift |
					     (std::uint64_t{1} << (shift - 1U))})
				for (const auto fpcr : std::array{
				     test::fpcr_fz16,
				     test::fpcr_ah,
				     test::fpcr_ah | test::fpcr_fz16,
				     test::fpcr_fz})
					check_half_flush(
						harness, form, input, fpcr, seed);
		}

		for (const auto shape : test::simd_shapes) {
			if (shape.width != 16U)
				continue;
			const test::Form form{
				test::Encoding::simd_fixed, unsigned_source,
				shape.scalar, 16U, 16U, shape.bytes, 16U};
			for (const auto fpcr : std::array{
			     test::fpcr_fz16,
			     test::fpcr_ah,
			     test::fpcr_ah | test::fpcr_fz16,
			     test::fpcr_fz})
				check_half_flush(harness, form, 1U, fpcr, seed);
		}
	}
}

void irrelevant_controls_and_sticky_status(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 4'000U;
	test::for_each_form([&](const test::Form &form) {
		const test::Registers registers{20U, 21U};
		auto initial = test::state(++seed);
		initial.fpcr = test::fpcr_fiz | test::fpcr_dn |
			test::fpcr_ahp |
			(form.destination_width == 16U ?
				test::fpcr_fz : test::fpcr_fz16);
		initial.fpsr = test::fpsr_overflow | test::fpsr_underflow |
			test::fpsr_inexact | test::fpsr_saturation;
		test::fill_source(initial, form, registers,
			positive_maximum(form) / 5U, true);
		test::check(harness,
			"integer-to-floating-irrelevant-controls-and-sticky-fpsr",
			form, registers, initial);
	}, false);
}

void vector_lanes_accumulate_exceptions(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 5'000U;
	for (const bool unsigned_source : {false, true}) {
		const test::Form form{
			test::Encoding::simd_integer, unsigned_source, false,
			16U, 16U, 16U, 0U};
		const test::Registers registers{24U, 25U};
		auto initial = test::state(++seed);
		const auto mask = test::low_mask(form.integer_width);
		for (unsigned lane = 0U; lane < test::lanes(form); ++lane) {
			const std::array values{
				std::uint64_t{0}, std::uint64_t{1},
				std::uint64_t{65'519}, std::uint64_t{65'520},
				std::uint64_t{65'535}, std::uint64_t{2'049},
				test::sign_mask(16U), mask};
			test::write_lane(initial.vectors[registers.source].bytes,
				lane, form.integer_width, values[lane]);
		}
		test::check(harness,
			"integer-to-floating-vector-accumulated-exceptions",
			form, registers, initial);
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	scalar_merging_and_vector_zeroing(harness);
	alternative_half_format_is_ignored(harness);
	half_output_flush_before_and_after_rounding(harness);
	irrelevant_controls_and_sticky_status(harness);
	vector_lanes_accumulate_exceptions(harness);
	harness.print_summary("arm64-integer-to-floating-control");
	return 0;
}
