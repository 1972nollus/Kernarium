// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_immediate_memory_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = uml::emulator::tests::base_immediate_memory;
namespace engine_tests = uml::emulator::tests;

namespace {

void reserved_unsigned(engine_tests::EngineHarness &harness)
{
	auto initial = test::seeded_state(0x8100'0000'0000'0000ULL);
	initial.registers[2] = test::data_base;
	initial.registers[3] = test::store_value;
	for (const auto [size_code, operation] :
	     {std::array{2U, 3U}, std::array{3U, 3U}})
		for (unsigned imm12 = 0; imm12 < 4096; ++imm12) {
			char name[88];
			std::snprintf(name, sizeof(name),
				"reserved unsigned size=%u op=%u imm=%u",
				size_code, operation, imm12);
			harness.check_illegal(name, test::encode_unsigned(
				size_code, operation, imm12, 2, 3), initial);
		}
}

void reserved_immediate(engine_tests::EngineHarness &harness)
{
	auto initial = test::seeded_state(0x8200'0000'0000'0000ULL);
	initial.registers[2] = test::data_base;
	initial.registers[3] = test::store_value;
	for (const auto [size_code, operation] :
	     {std::array{2U, 3U}, std::array{3U, 3U}})
		for (unsigned mode = 0; mode < 4; ++mode)
			for (unsigned imm9 = 0; imm9 < 512; ++imm9) {
				char name[96];
				std::snprintf(name, sizeof(name),
					"reserved immediate size=%u op=%u "
					"mode=%u imm=%u",
					size_code, operation, mode, imm9);
				harness.check_illegal(name, test::encode_other(
					size_code, operation, imm9,
					static_cast<test::Form>(mode),
					2, 3), initial);
			}
}

void reserved_prefetch_modes(engine_tests::EngineHarness &harness)
{
	auto initial = test::seeded_state(0x8300'0000'0000'0000ULL);
	initial.registers[2] = test::data_base;
	for (const auto form :
	     {test::Form::post_index, test::Form::unprivileged,
	      test::Form::pre_index})
		for (unsigned imm9 = 0; imm9 < 512; ++imm9) {
			char name[96];
			std::snprintf(name, sizeof(name),
				"reserved prefetch %.*s imm=%u",
				static_cast<int>(test::form_name(form).size()),
				test::form_name(form).data(), imm9);
			harness.check_illegal(name, test::encode_other(
				3, 2, imm9, form, 2, 3), initial);
		}
}

void register_fields_do_not_change_allocation(
	engine_tests::EngineHarness &harness)
{
	/* A misaligned SP makes ordering observable: the reserved size/opc
	 * combinations must be rejected during decode, before CheckSPAlignment.
	 */
	auto initial = test::seeded_state(0x8400'0000'0000'0000ULL);
	initial.sp = test::data_base + 1U;
	for (unsigned rn = 0; rn < 32; ++rn)
		for (unsigned rt = 0; rt < 32; ++rt) {
			char name[96];
			std::snprintf(name, sizeof(name),
				"reserved fields rn=%u rt=%u", rn, rt);
			harness.check_illegal(name,
				test::encode_other(2, 3, 0,
					test::Form::unscaled, rn, rt),
				initial);
		}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	reserved_unsigned(harness);
	reserved_immediate(harness);
	reserved_prefetch_modes(harness);
	register_fields_do_not_change_allocation(harness);
	test::require_engine_counts(harness, 14848U);
	harness.print_summary("arm64-base-immediate-memory-allocation");
	return 0;
}
