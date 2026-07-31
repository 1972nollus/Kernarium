// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_dot_engine_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_fp8_dot_engine_test;

namespace {

template<typename Function>
void for_each_form(Function &&function)
{
	for (const unsigned width : {16U, 32U})
		for (const bool wide : {false, true}) {
			function(test::Form{width, false, wide, 0U});
			const unsigned indices = width == 16U ? 8U : 4U;
			for (unsigned index = 0U; index < indices; ++index)
				function(test::Form{width, true, wide, index});
		}
}

void special_values_controls_and_unsupported_formats()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 70'000U;
	constexpr std::array fpcr_values{
		0U, test::source_oracle::fpcr_ah,
		1U << 0, 1U << 19, 1U << 24, 1U << 25,
		1U << 22, 2U << 22, 3U << 22,
		(3U << 22) | (1U << 24) |
			test::source_oracle::fpcr_ah,
	};
	for_each_form([&](const test::Form &form) {
		const test::Registers registers{
			28U, 29U,
			form.by_element && form.width == 16U ? 14U : 30U};
		for (const auto fpcr : fpcr_values)
			for (unsigned scenario = 0U; scenario < 8U;
			     ++scenario) {
				auto initial = test::state(++seed);
				initial.fpcr = fpcr;
				initial.fpsr = 0x0800'009dU;
				initial.fpmr = test::fpmr(
					scenario & 1U, (scenario >> 1U) & 1U,
					scenario * 3U,
					(scenario & 4U) != 0U);
				const std::uint8_t first =
					test::representative_inputs[
						12U + scenario];
				const std::uint8_t second =
					test::representative_inputs[
						(16U + scenario) %
						test::representative_inputs.size()];
				initial.vectors[29U].bytes.fill(
					static_cast<std::byte>(first));
				initial.vectors[registers.second].bytes.fill(
					static_cast<std::byte>(second));
				const auto &addends = form.width == 16U ?
					test::half_addends : test::single_addends;
				for (unsigned lane = 0U;
				     lane < test::lanes(form); ++lane)
					test::write_lane(
						initial.vectors[28U].bytes,
						lane, form.width,
						addends[(scenario + lane) %
							addends.size()]);
				char name[208];
				std::snprintf(name, sizeof(name),
					"fp8-dot-control width=%u indexed=%u wide=%u "
					"index=%u fpcr=%x scenario=%u",
					form.width,
					static_cast<unsigned>(form.by_element),
					static_cast<unsigned>(form.wide),
					form.index, fpcr, scenario);
				test::check(harness, name, form,
					registers, initial);
			}

		for (unsigned format1 = 0U; format1 < 8U; ++format1)
			for (unsigned format2 = 0U; format2 < 8U;
			     ++format2) {
				if (format1 < 2U && format2 < 2U)
					continue;
				auto initial = test::state(++seed);
				initial.fpcr =
					(seed & 1U) != 0U ?
						test::source_oracle::fpcr_ah : 0U;
				initial.fpsr = 0x0800'0018U;
				initial.fpmr = test::fpmr(
					format1, format2, 5U, true);
				initial.vectors[29U].bytes.fill(
					static_cast<std::byte>(0x3cU));
				initial.vectors[registers.second].bytes.fill(
					static_cast<std::byte>(0xbcU));
				test::check(harness,
					"fp8-dot-unsupported-source-format",
					form, registers, initial);
			}
	});
	harness.print_summary("arm64-fp8-dot-control");
}

void exact_cancellation_and_same_signed_zero()
{
	test::engine_tests::EngineHarness harness;
	for (const unsigned width : {16U, 32U}) {
		const test::Form form{width, false, width == 16U, 0U};
		auto initial = test::state(90'000U + width);
		initial.fpmr = test::fpmr();
		initial.vectors[28U].bytes.fill(std::byte{});
		initial.vectors[29U].bytes.fill(std::byte{});
		initial.vectors[30U].bytes.fill(std::byte{});
		initial.vectors[29U].bytes[0] =
			static_cast<std::byte>(0x3cU);
		initial.vectors[30U].bytes[0] =
			static_cast<std::byte>(0x3cU);
		initial.vectors[29U].bytes[1] =
			static_cast<std::byte>(0xbcU);
		initial.vectors[30U].bytes[1] =
			static_cast<std::byte>(0x3cU);
		if (width == 32U) {
			initial.vectors[29U].bytes[2] =
				static_cast<std::byte>(0x01U);
			initial.vectors[30U].bytes[2] =
				static_cast<std::byte>(0x01U);
		}
		test::check(harness, "fp8-dot-exact-cancellation",
			form, {28U, 29U, 30U}, initial);

		initial.vectors[28U].bytes.fill(std::byte{});
		initial.vectors[29U].bytes.fill(
			static_cast<std::byte>(0x80U));
		initial.vectors[30U].bytes.fill(
			static_cast<std::byte>(0x00U));
		for (unsigned lane = 0U; lane < test::lanes(form); ++lane)
			test::write_lane(initial.vectors[28U].bytes,
				lane, width,
				width == 16U ? 0x8000U : 0x8000'0000U);
		test::check(harness, "fp8-dot-negative-zero",
			form, {28U, 29U, 30U}, initial);
	}
	harness.print_summary("arm64-fp8-dot-exact-zero");
}

void fpmr_write_then_dot_in_one_generated_block()
{
	test::engine_tests::EngineHarness harness;
	const test::Form form{32U, true, true, 3U};
	const test::Registers registers{28U, 29U, 30U};
	auto initial = test::state(100'000U);
	const auto new_fpmr = test::fpmr(1U, 0U, 27U, true);
	initial.registers[5U] = new_fpmr;
	initial.vectors[29U].bytes.fill(
		static_cast<std::byte>(0x38U));
	initial.vectors[30U].bytes.fill(
		static_cast<std::byte>(0x3cU));
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
		.name = "fp8-dot-fpmr-write-same-block",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = test::native_requirements(form, initial.fpcr),
	});
	harness.print_summary("arm64-fp8-dot-fpmr-sequence");
}

} // namespace

int main()
{
	special_values_controls_and_unsupported_formats();
	exact_cancellation_and_same_signed_zero();
	fpmr_write_then_dot_in_one_generated_block();
	return 0;
}
