// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_convert_up_engine_test_support.hpp"
#include "arm64_basic_floating_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_fp8_convert_up_engine_test;
namespace basic = arm64_basic_floating_test;

namespace {

void alternate_mode_nan_sign_tininess_and_sticky_status()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 40'000U;
	constexpr std::array special{
		std::uint8_t{0x01U}, std::uint8_t{0x81U},
		std::uint8_t{0x7dU}, std::uint8_t{0xfdU},
		std::uint8_t{0x7fU}, std::uint8_t{0xffU},
	};
	for (const auto form : test::forms) {
		for (const unsigned format : {0U, 1U}) {
			for (const bool alternate : {false, true}) {
				for (const auto input : special) {
					auto initial = test::state(++seed);
					initial.fpcr = alternate ? test::fpcr_ah : 0U;
					initial.fpsr = 1U << 27;
					const unsigned value_scale =
						form.bfloat ? 63U : 15U;
					initial.fpmr = form.source2 ?
						test::fpmr(1U - format, format,
							7U, value_scale) :
						test::fpmr(format, 1U - format,
							value_scale, 31U);
					const std::array values{input};
					test::fill_source(initial, {28U, 29U}, values);
					char name[192];
					std::snprintf(name, sizeof(name),
						"fp8-convert-up-control bfloat=%u source2=%u "
						"upper=%u format=%u ah=%u input=%02x",
						static_cast<unsigned>(form.bfloat),
						static_cast<unsigned>(form.source2),
						static_cast<unsigned>(form.upper),
						format, static_cast<unsigned>(alternate),
						static_cast<unsigned>(input));
					test::check(harness, name, form, {28U, 29U}, initial);
				}
			}
		}
	}
	harness.print_summary("arm64-fp8-convert-up-control");
}

void unsupported_formats_and_ignored_controls()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 50'000U;
	constexpr std::array fpcr_values{
		0U, 1U, 2U, 3U,
		1U << 19, 1U << 24,
		(1U << 22) | (1U << 24),
		(3U << 22) | (1U << 19) | 3U,
	};
	for (const auto form : test::forms) {
		for (unsigned format = 2U; format < 8U; ++format) {
			for (const auto fpcr : fpcr_values) {
				auto initial = test::state(++seed);
				initial.fpcr = fpcr;
				initial.fpsr = test::fpsr_underflow | (1U << 27);
				initial.fpmr = form.source2 ?
					test::fpmr(0U, format, 0x55U, 0x3fU) :
					test::fpmr(format, 0U, 0x7fU, 0x2aU);
				test::fill_source(initial, {28U, 29U},
					test::representative_inputs);
				char name[176];
				std::snprintf(name, sizeof(name),
					"fp8-convert-up-unsupported bfloat=%u source2=%u "
					"upper=%u format=%u fpcr=%x",
					static_cast<unsigned>(form.bfloat),
					static_cast<unsigned>(form.source2),
					static_cast<unsigned>(form.upper),
					format, fpcr);
				test::check(harness, name, form, {28U, 29U}, initial);
			}
		}
	}
	harness.print_summary("arm64-fp8-convert-up-unsupported");
}

void fpmr_write_then_conversion_in_one_generated_block()
{
	test::engine_tests::EngineHarness harness;
	const test::Form form{false, true, true};
	const test::Registers registers{28U, 29U};
	auto initial = test::state(60'000U);
	const auto new_fpmr = test::fpmr(0U, 1U, 0U, 7U);
	initial.registers[5] = new_fpmr;
	constexpr std::array<std::uint8_t, 8> values{
		0x00U, 0x01U, 0x38U, 0x3cU,
		0x7eU, 0x7fU, 0x80U, 0xffU,
	};
	test::fill_source(initial, registers, values);
	auto architectural = initial;
	architectural.fpmr = new_fpmr;
	auto expected = test::expected_state(architectural, form, registers);
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
		.name = "fp8-convert-up-fpmr-write-same-block",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = test::native_requirements(),
	});
	harness.print_summary("arm64-fp8-convert-up-fpmr-sequence");
}

void native_fp_then_software_conversion_preserves_status()
{
	test::engine_tests::EngineHarness harness;
	const basic::Form add_form{
		basic::Operation::add, true, 32U, 16U};
	const basic::Registers add_registers{0U, 1U, 2U};
	const test::Form convert_form{false, false, false};
	const test::Registers convert_registers{4U, 3U};
	auto initial = test::state(61'000U);
	initial.fpcr = 0U;
	initial.fpsr = 0U;
	initial.fpmr = test::fpmr(0U, 0U, 0U, 0U);
	basic::set_lane_bits(initial, 1U, 0U, 32U, 0x3f80'0000U);
	basic::set_lane_bits(initial, 2U, 0U, 32U, 0x3f80'0000U);
	initial.vectors[3U].bytes.fill(std::byte{});
	initial.vectors[3U].bytes[0U] =
		static_cast<std::byte>(0x7dU);

	auto after_add = basic::expected_with_uniform_result(
		initial, add_form, add_registers, 0x4000'0000U);
	auto expected = test::expected_state(
		after_add, convert_form, convert_registers);
	expected.pc = test::engine_tests::engine_test_code_base + 12U;
	const std::array code{
		test::engine_tests::ProgramWord{
			.offset = 0U,
			.instruction =
				basic::instruction(add_form, add_registers)},
		test::engine_tests::ProgramWord{
			.offset = 4U,
			.instruction =
				test::instruction(
					convert_form, convert_registers)},
		test::engine_tests::ProgramWord{
			.offset = 8U, .instruction = 0xd400'0001U},
	};
	harness.check_program({
		.name = "fp8-convert-up-live-fp-status",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = test::native_requirements() |
			::uml::cpu::ArmFeature::floating_point,
	});
	harness.print_summary("arm64-fp8-convert-up-live-fp-status");
}

} // namespace

int main()
{
	alternate_mode_nan_sign_tininess_and_sticky_status();
	unsupported_formats_and_ignored_controls();
	fpmr_write_then_conversion_in_one_generated_block();
	native_fp_then_software_conversion_preserves_status();
	return 0;
}
