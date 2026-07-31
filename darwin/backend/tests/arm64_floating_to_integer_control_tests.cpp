// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_to_integer_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_floating_to_integer_test;

namespace {

inline constexpr std::uint32_t fpcr_exception_traps = 0x0000'9f00U;

void source_flush_controls_and_status_flags(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	test::for_each_nonfixed_form([&](const test::Form &form) {
		const auto representatives =
			test::representative_inputs(form.source_width);
		const std::array controls{
			std::uint32_t{0U},
			test::fpcr_fiz,
			test::fpcr_ah,
			test::fpcr_fz,
			test::fpcr_ah | test::fpcr_fz,
			test::fpcr_fz16,
			test::fpcr_ahp,
			test::fpcr_dn,
			test::fpcr_fiz | test::fpcr_ah | test::fpcr_fz |
				test::fpcr_fz16 | test::fpcr_ahp | test::fpcr_dn};
		for (unsigned control = 0U; control < controls.size(); ++control) {
			const test::Registers registers{4U, 5U};
			auto initial = test::state(++seed);
			initial.fpcr = controls[control];
			initial.fpsr = test::fpsr_saturation |
				((control & 1U) != 0U ? test::fpsr_inexact : 0U);
			const std::array inputs{
				representatives[2], representatives[3],
				representatives[4], representatives[5],
				representatives[18], representatives[19]};
			test::fill_source(initial, form, registers, inputs);
			char name[176];
			std::snprintf(name, sizeof(name),
				"floating-to-integer-control destination=%u rounding=%u "
				"unsigned=%u source=%u integer=%u control=%u",
				static_cast<unsigned>(form.destination),
				static_cast<unsigned>(form.rounding),
				static_cast<unsigned>(form.unsigned_result),
				form.source_width, form.integer_width, control);
			test::check(harness, name, form, registers, initial);
		}
	});
}

void scalar_merging_and_vector_zeroing(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 10'000U;
	test::for_each_form([&](const test::Form &form) {
		if (form.destination == test::Destination::gpr)
			return;
		const test::Registers registers{13U, 14U};
		auto initial = test::state(++seed);
		initial.fpcr = test::fpcr_nep;
		const std::array inputs{
			test::power_of_two(form.source_width, 0),
			test::power_of_two(form.source_width, 1, true)};
		test::fill_source(initial, form, registers, inputs);
		test::check(harness,
			form.scalar ?
				"floating-to-integer-scalar-nep-merge" :
				"floating-to-integer-vector-nep-ignored",
			form, registers, initial);
	});
}

void ignored_controls_sticky_flags_and_nonraising_trap_configuration(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 20'000U;
	test::for_each_nonfixed_form([&](const test::Form &form) {
		const test::Registers registers{16U, 17U};
		auto initial = test::state(++seed);
		initial.fpcr = test::fpcr_ahp | test::fpcr_dn |
			fpcr_exception_traps | (3U << 22);
		initial.fpsr = test::fpsr_invalid | test::fpsr_inexact |
			test::fpsr_input_denormal | test::fpsr_saturation;
		const std::array input{
			test::power_of_two(form.source_width, 0)};
		test::fill_source(initial, form, registers, input);
		test::check(harness,
			"floating-to-integer-ignored-controls-sticky-status-and-traps",
			form, registers, initial);
	});
}

void half_fz16_is_silent_and_ahp_is_ignored(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 30'000U;
	test::for_each_form([&](const test::Form &form) {
		if (form.source_width != 16U)
			return;
		for (const auto fpcr : std::array{
		     test::fpcr_fz16, test::fpcr_fz16 | test::fpcr_ahp,
		     test::fpcr_ahp, test::fpcr_ahp | test::fpcr_dn}) {
			const test::Registers registers{19U, 20U};
			auto initial = test::state(++seed);
			initial.fpcr = fpcr;
			const std::array inputs{
				std::uint64_t{0x0001U}, std::uint64_t{0x8001U},
				std::uint64_t{0x7c00U}, std::uint64_t{0x7e55U}};
			test::fill_source(initial, form, registers, inputs);
			test::check(harness,
				"floating-to-integer-half-fz16-and-ahp",
				form, registers, initial);
		}
	});
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	source_flush_controls_and_status_flags(harness);
	scalar_merging_and_vector_zeroing(harness);
	ignored_controls_sticky_flags_and_nonraising_trap_configuration(harness);
	half_fz16_is_silent_and_ahp_is_ignored(harness);
	harness.print_summary("arm64-floating-to-integer-control");
	return 0;
}
