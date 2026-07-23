// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_mops_test_support.hpp"
#include "arm64_mte_memory_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>

namespace mops = mops_test;
namespace mte = mte_memory_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr unsigned destination_register = 3;
constexpr unsigned count_register = 4;
constexpr unsigned source_register = 5;

void encoding_examples()
{
	mops::test::require(mops::set_tag_instruction(mops::Stage::prologue,
		0, 0, 1, 2) == 0x1dc2'0420U); // SETGP
	mops::test::require(mops::set_tag_instruction(mops::Stage::main,
		0, 3, 4, 5) == 0x1dc5'4483U); // SETGM
	mops::test::require(mops::set_tag_instruction(mops::Stage::epilogue,
		0, 6, 7, 8) == 0x1dc8'84e6U); // SETGE
	mops::test::require(mops::set_tag_instruction(mops::Stage::prologue,
		2, 9, 10, 11) == 0x1dcb'2549U); // SETGPN
	mops::test::require(mops::set_tag_instruction(mops::Stage::prologue,
		1, 12, 13, 14) == 0x1dce'15acU); // SETGPT
	mops::test::require(mops::set_tag_instruction(mops::Stage::prologue,
		3, 15, 16, 17) == 0x1dd1'360fU); // SETGPTN
}

void interpreter_all_options_stages_and_zero_source()
{
	constexpr std::uint64_t destination = mte::Memory::base + 4096;
	constexpr std::uint64_t logical_tag = 11;
	constexpr std::size_t size = 16 * 37;
	for (unsigned options = 0; options < 4; ++options)
		for (const unsigned source : {source_register, 31U}) {
			mte::Memory memory;
			std::fill_n(memory.at(destination), size, std::byte{0xcc});
			auto cpu = mops::state();
			cpu.registers[destination_register] = destination |
				(logical_tag << 56);
			cpu.registers[count_register] = size;
			cpu.registers[source_register] = 0x1234'5678'9abc'de5aULL;
			cpu.pc = mops::pc;
			const auto source_before = cpu.registers[source_register];
			const auto fill = source == 31 ? std::byte{} : std::byte{0x5a};
			for (const auto stage : {mops::Stage::prologue,
			     mops::Stage::main, mops::Stage::epilogue}) {
				const auto instruction = mops::set_tag_instruction(stage,
					options, destination_register, count_register, source);
				const auto instruction_pc = cpu.pc;
				mops::require_continued(arm64::Dispatcher{}.execute(
					cpu, memory, instruction), instruction_pc, instruction);
			}
			mops::test::require(cpu.pc == mops::pc + 12 &&
				cpu.nzcv == 0x2000'0000U &&
				cpu.registers[count_register] == 0 &&
				cpu.registers[destination_register] ==
					(destination | (logical_tag << 56)) + size &&
				cpu.registers[source_register] == source_before &&
				memory.reads == 0 && memory.writes == size &&
				memory.tag_reads == 0 && memory.tag_writes == size / 16 &&
				std::all_of(memory.at(destination),
					memory.at(destination) + size,
					[fill](std::byte value) { return value == fill; }));
			for (std::size_t offset = 0; offset < size; offset += 16)
				mops::test::require(memory.tag(destination + offset) ==
					logical_tag);
		}
}

void gadget_all_options_and_pages()
{
	constexpr std::uint64_t guest = 0x7f00'0000ULL;
	constexpr unsigned long generation = 0x4d4f5301UL;
	mops::test::StaticMapping mapping{
		guest, generation, "/tmp/uml-mops-setg-XXXXXX"};
	constexpr std::size_t size = 16 * 313;
	const auto destination = guest + 7 * 4096 + 0x130;
	for (unsigned options = 0; options < 4; ++options) {
		const unsigned source = (options & 1U) != 0 ? 31 : source_register;
		const auto fill = source == 31 ? std::byte{} :
			static_cast<std::byte>(0x70U + options);
		std::fill_n(mapping.at(destination), size, std::byte{0xcc});
		auto cpu = mops::state();
		cpu.registers[destination_register] = destination |
			(static_cast<std::uint64_t>(options + 7U) << 56);
		cpu.registers[count_register] = size;
		cpu.registers[source_register] = static_cast<unsigned>(fill);
		const auto source_before = cpu.registers[source_register];
		const std::array instructions{
			mops::set_tag_instruction(mops::Stage::prologue, options,
				destination_register, count_register, source),
			mops::set_tag_instruction(mops::Stage::main, options,
				destination_register, count_register, source),
			mops::set_tag_instruction(mops::Stage::epilogue, options,
				destination_register, count_register, source),
			0xd400'0001U};
		mops::test::require(mapping.execute(mops::pc, instructions, cpu) ==
			gadgets::Event::system_call && cpu.pc == mops::pc + 16 &&
			cpu.nzcv == 0x2000'0000U &&
			cpu.registers[count_register] == 0 &&
			cpu.registers[destination_register] ==
				(destination |
				 (static_cast<std::uint64_t>(options + 7U) << 56)) + size &&
			cpu.registers[source_register] == source_before &&
			std::all_of(mapping.at(destination),
				mapping.at(destination) + size,
				[fill](std::byte value) { return value == fill; }));
		for (std::size_t offset = 0; offset < size; offset += 16)
			mops::test::require(mte::static_load_tag(
				generation, destination + offset) == options + 7U);
	}
}

void register_matrix_and_reserved_encodings()
{
	mte::Memory memory;
	constexpr std::uint64_t guest = 0x8000'0000ULL;
	mops::test::StaticMapping mapping{
		guest, 0x4d4f5302UL, "/tmp/uml-mops-setg-reg-XXXXXX"};
	for (unsigned destination = 0; destination < 32; ++destination)
		for (unsigned count = 0; count < 32; ++count)
			for (unsigned source = 0; source < 32; ++source) {
				const unsigned options =
					(destination + count + source) & 3U;
				const auto instruction = mops::set_tag_instruction(
					mops::Stage::prologue, options,
					destination, count, source);
				const bool valid = mops::valid_set_registers(
					destination, count, source);
				auto interpreted = mops::state();
				interpreted.registers[destination % 31] = mte::Memory::base;
				interpreted.registers[count % 31] = 0;
				interpreted.pc = mops::pc;
				const auto result = arm64::Dispatcher{}.execute(
					interpreted, memory, instruction);
				mops::test::require(result.reason == (valid ?
					arm64::StopReason::continued :
					arm64::StopReason::illegal_instruction));

				auto gadget = mops::state();
				gadget.registers[destination % 31] = guest;
				gadget.registers[count % 31] = 0;
				mops::test::require(mapping.execute(mops::pc,
					instruction, gadget) == (valid ?
					gadgets::Event::system_call :
					gadgets::Event::illegal_instruction));
			}

	for (unsigned options = 0; options < 4; ++options)
		for (const auto reserved : {
		     mops::set_tag_instruction(mops::Stage::prologue, options,
			3, 4, 5) | 0x4000'0000U,
		     mops::set_tag_instruction(mops::Stage::prologue, options,
			3, 4, 5) | 0x8000'0000U,
		     mops::set_tag_instruction(mops::Stage::prologue, options,
			3, 4, 5) | 0xc000'0000U,
		     mops::set_tag_instruction(mops::Stage::prologue, options,
			3, 4, 5) | (3U << 14)}) {
			auto interpreted = mops::state();
			interpreted.pc = mops::pc;
			const auto before = interpreted;
			memory.reset_counts();
			mops::test::require(arm64::Dispatcher{}.execute(
				interpreted, memory, reserved).reason ==
				arm64::StopReason::illegal_instruction &&
				memory.reads == 0 && memory.writes == 0 &&
				memory.tag_reads == 0 && memory.tag_writes == 0);
			mops::require_state(interpreted, before, mops::pc);
			auto gadget = mops::state();
			const auto gadget_before = gadget;
			mops::test::require(mapping.execute(mops::pc, reserved, gadget) ==
				gadgets::Event::illegal_instruction);
			mops::require_state(gadget, gadget_before, mops::pc);
		}
}

void saturation_alignment_main_and_specialization()
{
	mte::Memory memory;
	const auto prologue = mops::set_tag_instruction(mops::Stage::prologue, 0,
		destination_register, count_register, source_register);
	auto saturated = mops::state();
	saturated.registers[destination_register] = mte::Memory::base;
	saturated.registers[count_register] = 0x8000'0000'0000'0000ULL;
	saturated.pc = mops::pc;
	mops::require_continued(arm64::Dispatcher{}.execute(
		saturated, memory, prologue), mops::pc, prologue);
	mops::test::require(saturated.registers[count_register] ==
		0x7fff'ffff'ffff'fff0ULL && saturated.nzcv == 0x2000'0000U);

	for (const auto [destination, count] : {
	     std::pair{mte::Memory::base + 1, std::uint64_t{16}},
	     std::pair{mte::Memory::base, std::uint64_t{17}}}) {
		auto cpu = mops::state();
		cpu.registers[destination_register] = destination;
		cpu.registers[count_register] = count;
		cpu.pc = mops::pc;
		const auto before = cpu;
		memory.reset_counts();
		const auto result = arm64::Dispatcher{}.execute(cpu, memory, prologue);
		mops::test::require(result.reason == arm64::StopReason::memory_fault &&
			result.address == (destination & mte::address_mask) && result.write &&
			memory.writes == 0 && memory.tag_writes == 0);
		mops::require_state(cpu, before, mops::pc);
	}

	auto zero = mops::state();
	zero.registers[destination_register] = mte::Memory::base + 1;
	zero.registers[count_register] = 0;
	zero.pc = mops::pc;
	const auto zero_destination = zero.registers[destination_register];
	mops::require_continued(arm64::Dispatcher{}.execute(
		zero, memory, prologue), mops::pc, prologue);
	mops::test::require(zero.registers[destination_register] ==
		zero_destination && zero.registers[count_register] == 0 &&
		zero.nzcv == 0x2000'0000U && memory.writes == 0 &&
		memory.tag_writes == 0);

	/* The selected main-stage size is zero. Its implementation-defined zero
	 * checks are false, so malformed option/alignment state is not observed. */
	auto main_cpu = mops::state();
	main_cpu.registers[destination_register] = mte::Memory::base + 1;
	main_cpu.registers[count_register] = 17;
	main_cpu.nzcv = 0xa000'0000U;
	main_cpu.pc = mops::pc;
	const auto main_before = main_cpu;
	const auto main = mops::set_tag_instruction(mops::Stage::main, 3,
		destination_register, count_register, source_register);
	mops::require_continued(arm64::Dispatcher{}.execute(
		main_cpu, memory, main), mops::pc, main);
	mops::require_state(main_cpu, main_before, mops::pc + 4);

#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	std::set<std::uintptr_t> entries;
	for (const auto stage : {mops::Stage::prologue, mops::Stage::main,
	     mops::Stage::epilogue})
		for (unsigned options = 0; options < 4; ++options)
			for (const unsigned source : {source_register, 31U}) {
				const auto built = gadgets::Builder{}.build(mops::pc,
					std::array{mops::set_tag_instruction(stage, options,
						destination_register, count_register, source)});
				mops::test::require(built.complete &&
					built.program.instruction_count() == 1);
				entries.insert(built.program.words().front());
			}
	mops::test::require(entries.size() == 4);
#endif
}

} // namespace

int main()
{
	encoding_examples();
	interpreter_all_options_stages_and_zero_source();
	gadget_all_options_and_pages();
	register_matrix_and_reserved_encodings();
	saturation_alignment_main_and_specialization();
}
