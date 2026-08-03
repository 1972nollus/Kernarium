// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_to_integer_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_floating_to_integer_test;

namespace {

[[nodiscard]] constexpr std::array<std::uint64_t, 12>
ties_and_neighbors(unsigned width) noexcept
{
	if (width == 16U)
		return {0x37ffU, 0x3800U, 0x3801U, 0xb7ffU, 0xb800U, 0xb801U,
			0x3dffU, 0x3e00U, 0x3e01U, 0xbdffU, 0xbe00U, 0xbe01U};
	if (width == 32U)
		return {0x3eff'ffffU, 0x3f00'0000U, 0x3f00'0001U,
			0xbeff'ffffU, 0xbf00'0000U, 0xbf00'0001U,
			0x3fbf'ffffU, 0x3fc0'0000U, 0x3fc0'0001U,
			0xbfbf'ffffU, 0xbfc0'0000U, 0xbfc0'0001U};
	return {0x3fdf'ffff'ffff'ffffULL, 0x3fe0'0000'0000'0000ULL,
		0x3fe0'0000'0000'0001ULL, 0xbfdf'ffff'ffff'ffffULL,
		0xbfe0'0000'0000'0000ULL, 0xbfe0'0000'0000'0001ULL,
		0x3ff7'ffff'ffff'ffffULL, 0x3ff8'0000'0000'0000ULL,
		0x3ff8'0000'0000'0001ULL, 0xbff7'ffff'ffff'ffffULL,
		0xbff8'0000'0000'0000ULL, 0xbff8'0000'0000'0001ULL};
}

void every_rounding_direction_for_every_nonfixed_form(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	test::for_each_nonfixed_form([&](const test::Form &form) {
		const auto inputs = ties_and_neighbors(form.source_width);
		for (unsigned index = 0U; index < inputs.size(); ++index) {
			const test::Registers registers{21U, 22U};
			auto initial = test::state(++seed);
			std::array<std::uint64_t, 8> lane_inputs{};
			for (unsigned lane = 0U; lane < lane_inputs.size(); ++lane)
				lane_inputs[lane] =
					inputs[(index + lane) % inputs.size()];
			test::fill_source(initial, form, registers, lane_inputs);
			char name[176];
			std::snprintf(name, sizeof(name),
				"floating-to-integer-rounding destination=%u rounding=%u "
				"unsigned=%u source=%u integer=%u tie=%u",
				static_cast<unsigned>(form.destination),
				static_cast<unsigned>(form.rounding),
				static_cast<unsigned>(form.unsigned_result),
				form.source_width, form.integer_width, index);
			test::check(harness, name, form, registers, initial);
		}
	});
}

void explicit_rounding_ignores_fpcr_rounding_mode(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 10'000U;
	test::for_each_nonfixed_form([&](const test::Form &form) {
		for (unsigned fpcr_rounding = 0U; fpcr_rounding < 4U;
		     ++fpcr_rounding) {
			const test::Registers registers{24U, 25U};
			auto initial = test::state(++seed);
			initial.fpcr = fpcr_rounding << 22;
			const auto inputs = ties_and_neighbors(form.source_width);
			test::fill_source(initial, form, registers, inputs);
			test::check(harness,
				"floating-to-integer-explicit-rounding-ignores-fpcr",
				form, registers, initial);
		}
	});
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_rounding_direction_for_every_nonfixed_form(harness);
	explicit_rounding_ignores_fpcr_rounding_mode(harness);
	harness.print_summary("arm64-floating-to-integer-rounding");
	return 0;
}
