// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_scale_test_support.hpp"

#include <array>

namespace test = arm64_floating_scale_test;

namespace {

void every_destructive_alias_and_register_edge(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	for (const auto &form : test::forms)
		for (const auto registers : test::register_cases) {
			auto initial = test::state(++seed);
			const auto format = test::format(form.width);
			test::fill_lanes(initial, registers.first, form,
				test::normal_bits(format,
					static_cast<unsigned>(format.bias),
					test::fraction_mask(format) / 3U));
			test::fill_lanes(initial, registers.second, form,
				test::scale_bits((seed & 1U) != 0U ? 3 : -2,
					form.width));
			harness.check_one("floating-scale-destructive-alias",
				test::instruction(form, registers), initial,
				test::expected_state(initial, form, registers),
				test::native_requirements());
		}
}

void back_to_back_translated_sequences(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 100U;
	for (const auto &form : test::forms) {
		std::array<test::engine_tests::ProgramWord,
			test::register_cases.size() + 1U> code{};
		auto initial = test::state(++seed);
		auto expected = initial;
		for (unsigned index = 0; index < test::register_cases.size();
		     ++index) {
			const auto registers = test::register_cases[index];
			code[index] = {
				.offset = index * sizeof(std::uint32_t),
				.instruction = test::instruction(form, registers),
			};
			expected = test::expected_state(expected, form, registers);
		}
		code.back() = {
			.offset = (code.size() - 1U) * sizeof(std::uint32_t),
			.instruction = 0xd400'0001U,
		};
		expected.pc = test::engine_tests::engine_test_code_base +
			code.size() * sizeof(std::uint32_t);
		harness.check_program({
			.name = "floating-scale-translated-sequence",
			.code = std::span<const test::engine_tests::ProgramWord>{code},
			.initial = initial,
			.expected = expected,
			.native = test::native_requirements(),
		});
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_destructive_alias_and_register_edge(harness);
	back_to_back_translated_sequences(harness);
	harness.print_summary("arm64-floating-scale-gadget");
	return 0;
}
