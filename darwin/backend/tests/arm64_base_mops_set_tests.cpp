// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_mops_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>

namespace mops = mops_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr unsigned destination_register = 3;
constexpr unsigned count_register = 4;
constexpr unsigned source_register = 5;

void encoding_examples()
{
	mops::test::require(mops::set_instruction(mops::Stage::prologue,
		0, 0, 1, 2) == 0x19c2'0420U);
	mops::test::require(mops::set_instruction(mops::Stage::main,
		0, 0, 1, 2) == 0x19c2'4420U);
	mops::test::require(mops::set_instruction(mops::Stage::epilogue,
		0, 0, 1, 2) == 0x19c2'8420U);
	mops::test::require(mops::set_instruction(mops::Stage::prologue,
		3, 3, 4, 5) == 0x19c5'3483U); // SETPTN
}

void interpreter_all_options_and_zero_source()
{
	constexpr std::uint64_t destination = mops::Memory::base + 0x4000;
	constexpr std::size_t size = 257;
	for (unsigned options = 0; options < 4; ++options) {
		for (const unsigned source : {source_register, 31U}) {
			mops::Memory memory;
			std::fill_n(memory.at(destination), size, std::byte{0xcc});
			auto cpu = mops::state();
			cpu.registers[destination_register] = destination;
			cpu.registers[count_register] = size;
			cpu.registers[source_register] = 0x1234'5678'9abc'de5aULL;
			cpu.pc = mops::pc;
			const auto source_before = cpu.registers[source_register];
			const auto fill = source == 31 ? std::byte{} : std::byte{0x5a};

			const auto prologue = mops::set_instruction(mops::Stage::prologue,
				options, destination_register, count_register, source);
			mops::require_continued(arm64::Dispatcher{}.execute(
				cpu, memory, prologue), mops::pc, prologue);
			mops::test::require(cpu.pc == mops::pc + 4 &&
				cpu.nzcv == 0x2000'0000U &&
				cpu.registers[count_register] == size &&
				cpu.registers[destination_register] == destination &&
				cpu.registers[source_register] == source_before &&
				memory.reads == 0 && memory.writes == 0);

			const auto main = mops::set_instruction(mops::Stage::main,
				options, destination_register, count_register, source);
			mops::require_continued(arm64::Dispatcher{}.execute(
				cpu, memory, main), mops::pc + 4, main);
			mops::test::require(cpu.pc == mops::pc + 8 &&
				cpu.registers[count_register] == size &&
				memory.reads == 0 && memory.writes == 0);

			const auto epilogue = mops::set_instruction(mops::Stage::epilogue,
				options, destination_register, count_register, source);
			mops::require_continued(arm64::Dispatcher{}.execute(
				cpu, memory, epilogue), mops::pc + 8, epilogue);
			mops::test::require(cpu.pc == mops::pc + 12 &&
				cpu.registers[count_register] == 0 &&
				cpu.registers[destination_register] == destination + size &&
				cpu.registers[source_register] == source_before &&
				std::all_of(memory.at(destination),
					memory.at(destination) + size,
					[fill](std::byte value) { return value == fill; }) &&
				memory.reads == 0 && memory.writes == size);
		}
	}
}

void gadget_all_options_and_pages()
{
	constexpr std::uint64_t guest = 0x7a00'0000ULL;
	mops::test::StaticMapping mapping{
		guest, 0x4d4f5201UL, "/tmp/uml-mops-set-XXXXXX"};
	constexpr std::size_t size = 5000;
	const auto destination =
		guest + 7 * mops::test::StaticMapping::page_size + 307;
	for (unsigned options = 0; options < 4; ++options) {
		const unsigned source = (options & 1U) != 0 ? 31 : source_register;
		const auto fill = source == 31 ? std::byte{} :
			static_cast<std::byte>(0x70U + options);
		std::fill_n(mapping.at(destination), size, std::byte{0xcc});
		auto cpu = mops::state();
		cpu.registers[destination_register] = destination;
		cpu.registers[count_register] = size;
		cpu.registers[source_register] = static_cast<unsigned>(fill);
		const auto source_before = cpu.registers[source_register];
		const std::array instructions{
			mops::set_instruction(mops::Stage::prologue, options,
				destination_register, count_register, source),
			mops::set_instruction(mops::Stage::main, options,
				destination_register, count_register, source),
			mops::set_instruction(mops::Stage::epilogue, options,
				destination_register, count_register, source),
			0xd400'0001U};
		mops::test::require(mapping.execute(mops::pc, instructions, cpu) ==
			gadgets::Event::system_call);
		mops::test::require(cpu.pc == mops::pc + 16 &&
			cpu.nzcv == 0x2000'0000U &&
			cpu.registers[count_register] == 0 &&
			cpu.registers[destination_register] == destination + size &&
			cpu.registers[source_register] == source_before &&
			std::all_of(mapping.at(destination),
				mapping.at(destination) + size,
				[fill](std::byte value) { return value == fill; }));
	}
}

void interpreter_fault_restart_and_format_checks()
{
	constexpr std::uint64_t destination = mops::Memory::base + 0x5000;
	constexpr std::uint64_t size = 32;
	mops::Memory memory;
	std::fill_n(memory.at(destination), size, std::byte{0xcc});
	constexpr unsigned completed = 13;
	memory.fault_write = destination + completed;
	auto cpu = mops::state();
	cpu.registers[destination_register] = destination;
	cpu.registers[count_register] = size;
	cpu.registers[source_register] = 0xa5;
	cpu.pc = mops::pc;
	for (const auto stage : {mops::Stage::prologue, mops::Stage::main}) {
		const auto encoded = mops::set_instruction(stage, 0,
			destination_register, count_register, source_register);
		const auto address = cpu.pc;
		mops::require_continued(arm64::Dispatcher{}.execute(
			cpu, memory, encoded), address, encoded);
	}
	const auto epilogue = mops::set_instruction(mops::Stage::epilogue, 0,
		destination_register, count_register, source_register);
	const auto fault = arm64::Dispatcher{}.execute(cpu, memory, epilogue);
	mops::test::require(fault.reason == arm64::StopReason::memory_fault &&
		fault.address == destination + completed && fault.write &&
		cpu.pc == mops::pc + 8 &&
		cpu.registers[count_register] == size - completed &&
		cpu.registers[destination_register] == destination + completed &&
		cpu.registers[source_register] == 0xa5 &&
		std::all_of(memory.at(destination),
			memory.at(destination) + completed,
			[](std::byte value) { return value == std::byte{0xa5}; }));

	for (const auto malformed : {
		std::pair{std::uint64_t{8}, 0U},
		std::pair{std::uint64_t{8}, 0xa000'0000U},
		std::pair{0x8000'0000'0000'0000ULL, 0x2000'0000U}}) {
		mops::Memory strict_memory;
		auto strict = mops::state();
		strict.registers[destination_register] = destination;
		strict.registers[count_register] = malformed.first;
		strict.registers[source_register] = 0x5a;
		strict.nzcv = malformed.second;
		strict.pc = mops::pc;
		const auto before = strict;
		const auto encoded = mops::set_instruction(mops::Stage::epilogue, 0,
			destination_register, count_register, source_register);
		mops::test::require(arm64::Dispatcher{}.execute(
			strict, strict_memory, encoded).reason ==
			arm64::StopReason::illegal_instruction);
		mops::require_state(strict, before, mops::pc);
		mops::test::require(strict_memory.reads == 0 &&
			strict_memory.writes == 0);
	}
}

void gadget_fault_restart()
{
	constexpr std::uint64_t guest = 0x7b00'0000ULL;
	mops::test::StaticMapping mapping{
		guest, 0x4d4f5202UL, "/tmp/uml-mops-set-fault-XXXXXX"};
	const auto destination = guest + mops::test::StaticMapping::length - 20;
	std::fill_n(mapping.at(destination), 20, std::byte{0xcc});
	auto cpu = mops::state();
	cpu.registers[destination_register] = destination;
	cpu.registers[count_register] = 64;
	cpu.registers[source_register] = 0x6d;
	const std::array instructions{
		mops::set_instruction(mops::Stage::prologue, 0,
			destination_register, count_register, source_register),
		mops::set_instruction(mops::Stage::main, 0,
			destination_register, count_register, source_register),
		mops::set_instruction(mops::Stage::epilogue, 0,
			destination_register, count_register, source_register),
		0xd400'0001U};
	mops::test::require(mapping.execute(mops::pc, instructions, cpu) ==
		gadgets::Event::memory_fault);
	mops::test::require(cpu.pc == mops::pc + 8 &&
		cpu.registers[count_register] == 44 &&
		cpu.registers[destination_register] == destination + 20 &&
		cpu.registers[source_register] == 0x6d &&
		std::all_of(mapping.at(destination), mapping.at(destination) + 20,
			[](std::byte value) { return value == std::byte{0x6d}; }));
}

void register_and_reserved_matrices()
{
	constexpr std::uint64_t guest = 0x7c00'0000ULL;
	mops::test::StaticMapping mapping{
		guest, 0x4d4f5203UL, "/tmp/uml-mops-set-reg-XXXXXX"};
	mops::Memory memory;
	for (unsigned destination = 0; destination < 32; ++destination)
		for (unsigned count = 0; count < 32; ++count)
			for (unsigned source = 0; source < 32; ++source) {
				const auto encoded = mops::set_instruction(
					mops::Stage::prologue, 0, destination, count, source);
				const bool valid = mops::valid_set_registers(
					destination, count, source);
				auto interpreted = mops::state();
				interpreted.registers[destination % 31] =
					mops::Memory::base;
				if (source != 31)
					interpreted.registers[source] = 0x5a;
				interpreted.registers[count % 31] = 0;
				interpreted.pc = mops::pc;
				const auto result = arm64::Dispatcher{}.execute(
					interpreted, memory, encoded);
				mops::test::require(result.reason == (valid ?
					arm64::StopReason::continued :
					arm64::StopReason::illegal_instruction));

				auto gadget = mops::state();
				gadget.registers[destination % 31] = guest;
				if (source != 31)
					gadget.registers[source] = 0x5a;
				gadget.registers[count % 31] = 0;
				mops::test::require(mapping.execute(mops::pc, encoded, gadget) ==
					(valid ? gadgets::Event::system_call :
					 gadgets::Event::illegal_instruction));
			}

	for (unsigned options = 0; options < 4; ++options)
		for (const auto reserved : {mops::set_instruction(
				mops::Stage::prologue, options, 3, 4, 5) |
					0x4000'0000U,
			mops::set_instruction(mops::Stage::prologue, options,
				3, 4, 5) | 0x8000'0000U,
			mops::set_instruction(mops::Stage::prologue, options,
				3, 4, 5) | 0xc000'0000U,
			mops::set_instruction(mops::Stage::prologue, options,
				3, 4, 5) | (3U << 14)}) {
			auto cpu = mops::state();
			cpu.pc = mops::pc;
			mops::test::require(arm64::Dispatcher{}.execute(
				cpu, memory, reserved).reason ==
				arm64::StopReason::illegal_instruction);
			mops::test::require(mapping.execute(mops::pc, reserved, cpu) ==
				gadgets::Event::illegal_instruction);
		}
}

void saturation_and_specialization()
{
	constexpr std::uint64_t guest = 0x7d00'0000ULL;
	mops::test::StaticMapping mapping{
		guest, 0x4d4f5204UL, "/tmp/uml-mops-set-sat-XXXXXX"};
	mops::Memory memory;
	const auto encoded = mops::set_instruction(mops::Stage::prologue, 0,
		destination_register, count_register, source_register);
	auto interpreted = mops::state();
	interpreted.registers[count_register] = 0x8000'0000'0000'0000ULL;
	interpreted.pc = mops::pc;
	mops::require_continued(arm64::Dispatcher{}.execute(
		interpreted, memory, encoded), mops::pc, encoded);
	mops::test::require(interpreted.registers[count_register] ==
		0x7fff'ffff'ffff'ffffULL && interpreted.nzcv == 0x2000'0000U);
	auto gadget = mops::state();
	gadget.registers[count_register] = 0x8000'0000'0000'0000ULL;
	mops::test::require(mapping.execute(mops::pc, encoded, gadget) ==
		gadgets::Event::system_call);
	mops::test::require(gadget.registers[count_register] ==
		0x7fff'ffff'ffff'ffffULL && gadget.nzcv == 0x2000'0000U);

#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	std::set<std::uintptr_t> entries;
	for (const auto stage : {mops::Stage::prologue, mops::Stage::main,
		mops::Stage::epilogue})
		for (const unsigned source : {source_register, 31U}) {
			const auto built = gadgets::Builder{}.build(mops::pc,
				std::array{mops::set_instruction(stage, 0,
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
	interpreter_all_options_and_zero_source();
	gadget_all_options_and_pages();
	interpreter_fault_restart_and_format_checks();
	gadget_fault_restart();
	register_and_reserved_matrices();
	saturation_and_specialization();
}
