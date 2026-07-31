// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_convert_down_engine_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_fp8_convert_down_engine_test;

namespace {

void fpcr_controls_unsupported_formats_and_sticky_status()
{
	test::engine_tests::EngineHarness harness;
	constexpr std::array controls{
		0U, 1U, 2U, 3U, 1U << 19, 1U << 24,
		(1U << 22) | (1U << 24),
		(2U << 22) | (1U << 19) | 1U,
		(3U << 22) | 2U,
	};
	unsigned seed = 3'000U;
	for (const auto form : test::forms) {
		const auto inputs = test::representative_inputs(form.source_width);
		for (const auto fpcr : controls) {
			for (const unsigned index : {2U, 14U, 16U, 17U}) {
				auto initial = test::state(++seed);
				initial.fpcr = fpcr;
				initial.fpsr = test::fpsr_overflow | (1U << 27);
				const std::array first{inputs[index]};
				const std::array second{inputs[index ^ 1U]};
				test::fill_source(initial, form, {28U, 29U, 30U},
					first, second);
				char name[176];
				std::snprintf(name, sizeof(name),
					"fp8-convert-down-control source=%u upper=%u "
					"fpcr=%x input=%u",
					form.source_width,
					static_cast<unsigned>(form.upper), fpcr, index);
				test::check(harness, name, form, {28U, 29U, 30U},
					initial);
			}
		}
		for (unsigned format = 2U; format < 8U; ++format) {
			auto initial = test::state(++seed);
			initial.fpmr = test::fpmr(format);
			const std::array<std::uint32_t, 1> values{
				form.source_width == 16U ? 0x3c00U :
					0x3f80'0000U};
			test::fill_source(initial, form, {28U, 29U, 30U},
				values, values);
			test::check(harness, "fp8-convert-down-unsupported-format",
				form, {28U, 29U, 30U}, initial);
		}
	}
	harness.print_summary("arm64-fp8-convert-down-control");
}

void fpmr_write_then_conversion_in_one_generated_block()
{
	test::engine_tests::EngineHarness harness;
	const test::Form form{16U, false};
	const test::Registers registers{28U, 29U, 30U};
	auto initial = test::state(4'000U);
	const std::uint64_t new_fpmr = test::fpmr(1U, 1);
	initial.registers[5] = new_fpmr;
	const std::array<std::uint32_t, 1> first{0x3c00U};
	const std::array<std::uint32_t, 1> second{0xc000U};
	test::fill_source(initial, form, registers, first, second);
	auto architectural = initial;
	architectural.fpmr = new_fpmr;
	auto expected =
		test::expected_state(architectural, form, registers);
	expected.pc = test::engine_tests::engine_test_code_base + 12U;
	const std::array code{
		test::engine_tests::ProgramWord{
			.offset = 0U, .instruction = 0xd51b'4445U},
		test::engine_tests::ProgramWord{
			.offset = 4U,
			.instruction = test::instruction(form, registers)},
		test::engine_tests::ProgramWord{
			.offset = 8U, .instruction = 0xd400'0001U},
	};
	harness.check_program({
		.name = "fp8-fpmr-write-then-convert-same-block",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = test::native_requirements(),
	});
	harness.print_summary("arm64-fp8-convert-down-fpmr-sequence");
}

} // namespace

int main()
{
	fpcr_controls_unsupported_formats_and_sticky_status();
	fpmr_write_then_conversion_in_one_generated_block();
	return 0;
}
