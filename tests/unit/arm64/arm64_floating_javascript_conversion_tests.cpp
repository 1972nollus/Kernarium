// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_to_integer_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_floating_to_integer_test;

namespace {

inline constexpr std::uint32_t fpcr_exception_traps = 0x0000'9f00U;

void register_allocation_and_values(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	const auto representatives = test::representative_inputs(64U);
	const std::array extra{
		std::uint64_t{0x41df'ffff'ffc0'0000ULL},
		std::uint64_t{0x41e0'0000'0000'0000ULL},
		std::uint64_t{0x41f0'0000'0010'0000ULL},
		std::uint64_t{0xc1e0'0000'0000'0000ULL},
		std::uint64_t{0xc1f0'0000'0010'0000ULL},
		std::uint64_t{0x3ff0'0000'0000'0000ULL},
		std::uint64_t{0xbff0'0000'0000'0000ULL}};
	for (const auto registers : test::register_edges) {
		for (unsigned index = 0U;
		     index < representatives.size() + extra.size(); ++index) {
			auto initial = test::state(++seed);
			const auto input = index < representatives.size() ?
				representatives[index] :
				extra[index - representatives.size()];
			test::write_lane(initial.vectors[registers.source].bytes,
				0U, 64U, input);
			char name[144];
			std::snprintf(name, sizeof(name),
				"floating-javascript-conversion rd=%u rn=%u input=%u",
				registers.destination, registers.source, index);
			test::check_javascript(harness, name, registers, initial);
		}
	}
}

void controls_flags_and_exact_indicator(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 2'000U;
	const auto representatives = test::representative_inputs(64U);
	const std::array controls{
		std::uint32_t{0U}, test::fpcr_fiz, test::fpcr_ah,
		test::fpcr_fz, test::fpcr_ah | test::fpcr_fz,
		test::fpcr_dn, test::fpcr_ahp, test::fpcr_nep,
		std::uint32_t{3U << 22}};
	for (unsigned control = 0U; control < controls.size(); ++control) {
		for (const unsigned input_index : {0U, 1U, 2U, 3U, 12U, 13U,
		     14U, 15U, 16U, 17U, 18U, 19U}) {
			const test::Registers registers{8U, 9U};
			auto initial = test::state(++seed);
			initial.fpcr = controls[control];
			initial.fpsr = test::fpsr_saturation |
				((input_index & 1U) != 0U ?
					test::fpsr_inexact : 0U);
			test::write_lane(initial.vectors[registers.source].bytes,
				0U, 64U, representatives[input_index]);
			char name[128];
			std::snprintf(name, sizeof(name),
				"floating-javascript-control=%u input=%u",
				control, input_index);
			test::check_javascript(harness, name, registers, initial);
		}
	}

	const test::Registers registers{8U, 9U};
	auto initial = test::state(++seed);
	initial.fpcr = fpcr_exception_traps;
	test::write_lane(initial.vectors[registers.source].bytes, 0U, 64U,
		0x3ff0'0000'0000'0000ULL);
	test::check_javascript(harness,
		"floating-javascript-nonraising-trap-configuration",
		registers, initial);
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	register_allocation_and_values(harness);
	controls_flags_and_exact_indicator(harness);
	harness.print_summary("arm64-floating-javascript-conversion");
	return 0;
}
