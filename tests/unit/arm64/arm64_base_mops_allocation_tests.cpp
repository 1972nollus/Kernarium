// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_mops_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mops = mops_test;
namespace engine = uml::emulator::tests;

namespace {

constexpr unsigned destination_register = 3;
constexpr unsigned source_register = 4;
constexpr unsigned count_register = 5;

void encoding_examples()
{
	mops::require(mops::copy_instruction(false, mops::Stage::prologue,
		0, 0, 1, 2) == 0x1901'0440U, "CPYFP encoding");
	mops::require(mops::copy_instruction(false, mops::Stage::main,
		0, 0, 1, 2) == 0x1941'0440U, "CPYFM encoding");
	mops::require(mops::copy_instruction(false, mops::Stage::epilogue,
		0, 0, 1, 2) == 0x1981'0440U, "CPYFE encoding");
	mops::require(mops::copy_instruction(false, mops::Stage::prologue,
		4, 3, 4, 5) == 0x1904'44a3U, "CPYFPWN encoding");
	mops::require(mops::copy_instruction(true, mops::Stage::prologue,
		0, 0, 1, 2) == 0x1d01'0440U, "CPYP encoding");
	mops::require(mops::copy_instruction(true, mops::Stage::main,
		0, 0, 1, 2) == 0x1d41'0440U, "CPYM encoding");
	mops::require(mops::copy_instruction(true, mops::Stage::epilogue,
		0, 0, 1, 2) == 0x1d81'0440U, "CPYE encoding");
	mops::require(mops::copy_instruction(true, mops::Stage::prologue,
		5, 3, 4, 5) == 0x1d04'54a3U, "CPYPWTWN encoding");
	mops::require(mops::set_instruction(mops::Stage::prologue,
		0, 0, 1, 2) == 0x19c2'0420U, "SETP encoding");
	mops::require(mops::set_instruction(mops::Stage::main,
		0, 0, 1, 2) == 0x19c2'4420U, "SETM encoding");
	mops::require(mops::set_instruction(mops::Stage::epilogue,
		0, 0, 1, 2) == 0x19c2'8420U, "SETE encoding");
	mops::require(mops::set_instruction(mops::Stage::prologue,
		3, 3, 4, 5) == 0x19c5'3483U, "SETPTN encoding");
	mops::require(mops::set_tag_instruction(mops::Stage::prologue,
		0, 0, 1, 2) == 0x1dc2'0420U, "SETGP encoding");
	mops::require(mops::set_tag_instruction(mops::Stage::main,
		0, 3, 4, 5) == 0x1dc5'4483U, "SETGM encoding");
	mops::require(mops::set_tag_instruction(mops::Stage::epilogue,
		0, 6, 7, 8) == 0x1dc8'84e6U, "SETGE encoding");
	mops::require(mops::set_tag_instruction(mops::Stage::prologue,
		3, 15, 16, 17) == 0x1dd1'360fU, "SETGPTN encoding");
}

void allocation_table(engine::EngineHarness &harness)
{
	std::size_t allocated = 0;
	std::size_t reserved = 0;
	for (unsigned size = 0; size < 4; ++size)
		for (unsigned family = 0; family < 2; ++family)
			for (unsigned stage = 0; stage < 4; ++stage)
				for (unsigned options = 0; options < 16; ++options) {
					const auto instruction = mops::raw_instruction(size,
						family, stage, options, destination_register,
						source_register, count_register);
					auto initial = mops::state();
					initial.registers[destination_register] =
						mops::data_base;
					initial.registers[source_register] =
						mops::data_base + mops::page_size;
					initial.registers[count_register] = 0;
					if (!mops::allocated_raw(size, stage, options)) {
						harness.check_illegal("reserved MOPS allocation",
							instruction, initial);
						++reserved;
						continue;
					}

					auto expected = initial;
					const bool prologue = stage < 3U ?
						stage == 0U : (options >> 2) == 0U;
					if (prologue)
						expected.nzcv = mops::option_b_forward;
					harness.check_one("allocated MOPS cell", instruction,
						initial, expected,
						mops::native(stage == 3U && family == 1U));
					++allocated;
				}
	mops::require(allocated == 120, "allocated MOPS table cell count");
	mops::require(reserved == 392, "reserved MOPS table cell count");
}

void copy_register_allocations(engine::EngineHarness &harness, bool general)
{
	for (unsigned destination = 0; destination < 32; ++destination)
		for (unsigned source = 0; source < 32; ++source)
			for (unsigned count = 0; count < 32; ++count) {
				const auto instruction = mops::copy_instruction(general,
					mops::Stage::prologue,
					(destination + source + count) & 15U,
					destination, source, count);
				auto initial = mops::state();
				if (destination < 31U)
					initial.registers[destination] = mops::data_base;
				if (source < 31U)
					initial.registers[source] =
						mops::data_base + mops::page_size;
				if (count < 31U)
					initial.registers[count] = 0;
				if (!mops::valid_copy_registers(destination, source,
					count)) {
					harness.check_illegal("copy register allocation",
						instruction, initial);
					continue;
				}
				auto expected = initial;
				expected.nzcv = mops::option_b_forward;
				harness.check_one("copy register allocation", instruction,
					initial, expected, mops::native());
			}
}

void set_register_allocations(engine::EngineHarness &harness, bool tags)
{
	for (unsigned destination = 0; destination < 32; ++destination)
		for (unsigned count = 0; count < 32; ++count)
			for (unsigned source = 0; source < 32; ++source) {
				const auto instruction = tags ?
					mops::set_tag_instruction(mops::Stage::prologue,
						(destination + count + source) & 3U,
						destination, count, source) :
					mops::set_instruction(mops::Stage::prologue,
						(destination + count + source) & 3U,
						destination, count, source);
				auto initial = mops::state();
				if (destination < 31U)
					initial.registers[destination] = mops::data_base;
				if (count < 31U)
					initial.registers[count] = 0;
				if (source < 31U)
					initial.registers[source] = 0x5a;
				if (!mops::valid_set_registers(destination, count,
					source)) {
					harness.check_illegal("set register allocation",
						instruction, initial);
					continue;
				}
				auto expected = initial;
				expected.nzcv = mops::option_b_forward;
				harness.check_one("set register allocation", instruction,
					initial, expected, mops::native(tags));
			}
}

} // namespace

int main()
{
	encoding_examples();
	engine::EngineHarness harness;
	allocation_table(harness);
	copy_register_allocations(harness, false);
	copy_register_allocations(harness, true);
	set_register_allocations(harness, false);
	set_register_allocations(harness, true);
	harness.print_summary("arm64-base-mops-allocation");
}
