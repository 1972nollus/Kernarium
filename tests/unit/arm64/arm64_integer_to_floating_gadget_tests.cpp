// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_integer_to_floating_test_support.hpp"

#include <array>
#include <span>

namespace test = arm64_integer_to_floating_test;

namespace {

void every_destructive_alias_and_register_edge(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		for (const auto registers : test::register_edges) {
			auto initial = test::state(++seed);
			test::fill_source(initial, form, registers,
				0x8123'4567'89ab'cdefULL, true);
			test::check(harness,
				"integer-to-floating-gadget-register-alias",
				form, registers, initial);
		}
	}, false);
}

void every_gpr_zero_source_specialization(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 2'000U;
	test::for_each_form([&](const test::Form &form) {
		if (!test::source_is_gpr(form))
			return;
		for (const unsigned destination : {0U, 17U, 31U}) {
			const test::Registers registers{destination, 31U};
			auto initial = test::state(++seed);
			test::check(harness,
				"integer-to-floating-gadget-zero-register-source",
				form, registers, initial);
		}
	});
}

void back_to_back_translated_sequences(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 10'000U;
	test::for_each_form([&](const test::Form &form) {
		std::array<test::engine_tests::ProgramWord,
			test::register_edges.size() + 1U> code{};
		auto initial = test::state(++seed);
		for (unsigned index = 0U; index < test::register_edges.size();
		     ++index) {
			const auto registers = test::register_edges[index];
			test::fill_source(initial, form, registers,
				0x7fed'cba9'8765'4321ULL ^
					(static_cast<std::uint64_t>(index) *
						0x1111'1111'1111'1111ULL),
				true);
			code[index] = {
				.offset = index * sizeof(std::uint32_t),
				.instruction = test::instruction(form, registers),
			};
		}
		code.back() = {
			.offset = (code.size() - 1U) * sizeof(std::uint32_t),
			.instruction = 0xd400'0001U,
		};

		auto expected = initial;
		for (const auto registers : test::register_edges)
			expected = test::expected_state(expected, form, registers);
		expected.pc = test::engine_tests::engine_test_code_base +
			code.size() * sizeof(std::uint32_t);
		harness.check_program({
			.name = "integer-to-floating-back-to-back-translated-sequence",
			.code =
				std::span<const test::engine_tests::ProgramWord>{code},
			.initial = initial,
			.expected = expected,
			.native = test::native_requirements(form),
		});
	}, false);
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_destructive_alias_and_register_edge(harness);
	every_gpr_zero_source_specialization(harness);
	back_to_back_translated_sequences(harness);
	harness.print_summary("arm64-integer-to-floating-gadget");
	return 0;
}
