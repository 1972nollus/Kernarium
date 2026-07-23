// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_mops_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace mops = mops_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr unsigned destination_register = 3;
constexpr unsigned source_register = 4;
constexpr unsigned count_register = 5;

void encoding_examples()
{
	mops::test::require(mops::copy_instruction(false, mops::Stage::prologue,
		0, 0, 1, 2) == 0x1901'0440U);
	mops::test::require(mops::copy_instruction(false, mops::Stage::main,
		0, 0, 1, 2) == 0x1941'0440U);
	mops::test::require(mops::copy_instruction(false, mops::Stage::epilogue,
		0, 0, 1, 2) == 0x1981'0440U);
	mops::test::require(mops::copy_instruction(false, mops::Stage::prologue,
		4, 3, 4, 5) == 0x1904'44a3U); // CPYFPWN
	mops::test::require(mops::copy_instruction(false, mops::Stage::prologue,
		2, 6, 7, 8) == 0x1907'2506U); // CPYFPRT
}

void interpreter_all_options()
{
	constexpr std::uint64_t source = mops::Memory::base + 0x1000;
	constexpr std::uint64_t destination = mops::Memory::base + 0x5000;
	constexpr std::size_t size = 257;
	for (unsigned options = 0; options < 16; ++options) {
		mops::Memory memory;
		mops::fill_pattern(std::span{memory.at(source), size}, options);
		std::fill_n(memory.at(destination), size, std::byte{0xcc});
		auto cpu = mops::state();
		cpu.registers[destination_register] = destination;
		cpu.registers[source_register] = source;
		cpu.registers[count_register] = size;
		cpu.pc = mops::pc;

		const auto prologue = mops::copy_instruction(false,
			mops::Stage::prologue, options, destination_register,
			source_register, count_register);
		mops::require_continued(arm64::Dispatcher{}.execute(
			cpu, memory, prologue), mops::pc, prologue);
		mops::test::require(cpu.pc == mops::pc + 4 &&
			cpu.nzcv == 0x2000'0000U &&
			cpu.registers[count_register] == size &&
			cpu.registers[source_register] == source &&
			cpu.registers[destination_register] == destination &&
			memory.reads == 0 && memory.writes == 0);

		const auto main = mops::copy_instruction(false, mops::Stage::main,
			options, destination_register, source_register, count_register);
		mops::require_continued(arm64::Dispatcher{}.execute(
			cpu, memory, main), mops::pc + 4, main);
		mops::test::require(cpu.pc == mops::pc + 8 &&
			cpu.registers[count_register] == size &&
			memory.reads == 0 && memory.writes == 0);

		const auto epilogue = mops::copy_instruction(false,
			mops::Stage::epilogue, options, destination_register,
			source_register, count_register);
		mops::require_continued(arm64::Dispatcher{}.execute(
			cpu, memory, epilogue), mops::pc + 8, epilogue);
		mops::test::require(cpu.pc == mops::pc + 12 &&
			cpu.registers[count_register] == 0 &&
			cpu.registers[source_register] == source + size &&
			cpu.registers[destination_register] == destination + size &&
			std::equal(memory.at(source), memory.at(source) + size,
				memory.at(destination)) &&
			memory.reads == size && memory.writes == size);
	}
}

void gadget_all_options_and_pages()
{
	constexpr std::uint64_t guest = 0x7400'0000ULL;
	mops::test::StaticMapping mapping{
		guest, 0x4d4f5001UL, "/tmp/uml-mops-cpyf-XXXXXX"};
	constexpr std::size_t size = 5000;
	const auto source = guest + 2 * mops::test::StaticMapping::page_size + 123;
	const auto destination = guest + 8 * mops::test::StaticMapping::page_size + 311;
	for (unsigned options = 0; options < 16; ++options) {
		mops::fill_pattern(std::span{mapping.at(source), size}, options + 7);
		std::vector<std::byte> expected(
			mapping.at(source), mapping.at(source) + size);
		std::fill_n(mapping.at(destination), size, std::byte{0xcc});
		auto cpu = mops::state();
		cpu.registers[destination_register] = destination;
		cpu.registers[source_register] = source;
		cpu.registers[count_register] = size;
		const std::array instructions{
			mops::copy_instruction(false, mops::Stage::prologue, options,
				destination_register, source_register, count_register),
			mops::copy_instruction(false, mops::Stage::main, options,
				destination_register, source_register, count_register),
			mops::copy_instruction(false, mops::Stage::epilogue, options,
				destination_register, source_register, count_register),
			0xd400'0001U};
		mops::test::require(mapping.execute(mops::pc, instructions, cpu) ==
			gadgets::Event::system_call);
		mops::test::require(std::equal(expected.begin(), expected.end(),
			mapping.at(destination)) && cpu.pc == mops::pc + 16 &&
			cpu.nzcv == 0x2000'0000U &&
			cpu.registers[count_register] == 0 &&
			cpu.registers[source_register] == source + size &&
			cpu.registers[destination_register] == destination + size);
	}
}

void interpreter_fault_restart_and_format_checks()
{
	constexpr std::uint64_t source = mops::Memory::base + 0x1000;
	constexpr std::uint64_t destination = mops::Memory::base + 0x5000;
	constexpr std::uint64_t size = 32;
	for (const bool write_fault : {false, true}) {
		mops::Memory memory;
		mops::fill_pattern(std::span{memory.at(source), size}, 11);
		std::fill_n(memory.at(destination), size, std::byte{0xcc});
		const unsigned completed = write_fault ? 11U : 7U;
		if (write_fault)
			memory.fault_write = destination + completed;
		else
			memory.fault_read = source + completed;
		auto cpu = mops::state();
		cpu.registers[destination_register] = destination;
		cpu.registers[source_register] = source;
		cpu.registers[count_register] = size;
		cpu.pc = mops::pc;
		for (const auto stage : {mops::Stage::prologue, mops::Stage::main}) {
			const auto encoded = mops::copy_instruction(false, stage, 0,
				destination_register, source_register, count_register);
			const auto address = cpu.pc;
			mops::require_continued(arm64::Dispatcher{}.execute(
				cpu, memory, encoded), address, encoded);
		}
		const auto epilogue = mops::copy_instruction(false,
			mops::Stage::epilogue, 0, destination_register,
			source_register, count_register);
		const auto fault = arm64::Dispatcher{}.execute(cpu, memory, epilogue);
		mops::test::require(fault.reason == arm64::StopReason::memory_fault &&
			fault.address == (write_fault ? destination : source) + completed &&
			fault.write == write_fault && cpu.pc == mops::pc + 8 &&
			cpu.registers[count_register] == size - completed &&
			cpu.registers[source_register] == source + completed &&
			cpu.registers[destination_register] == destination + completed &&
			std::equal(memory.at(source), memory.at(source) + completed,
				memory.at(destination)));
	}

	mops::Memory memory;
	auto malformed = mops::state();
	malformed.registers[destination_register] = destination;
	malformed.registers[source_register] = source;
	malformed.registers[count_register] = 8;
	malformed.nzcv = 0;
	malformed.pc = mops::pc;
	const auto before = malformed;
	const auto encoded = mops::copy_instruction(false, mops::Stage::epilogue,
		0, destination_register, source_register, count_register);
	mops::test::require(arm64::Dispatcher{}.execute(
		malformed, memory, encoded).reason ==
		arm64::StopReason::illegal_instruction);
	mops::require_state(malformed, before, mops::pc);
	mops::test::require(memory.reads == 0 && memory.writes == 0);
}

void gadget_fault_restart()
{
	constexpr std::uint64_t guest = 0x7500'0000ULL;
	mops::test::StaticMapping mapping{
		guest, 0x4d4f5002UL, "/tmp/uml-mops-cpyf-fault-XXXXXX"};
	const auto source = guest + mops::test::StaticMapping::length - 20;
	const auto destination = guest + 2 * mops::test::StaticMapping::page_size;
	mops::fill_pattern(std::span{mapping.at(source), 20}, 13);
	std::fill_n(mapping.at(destination), 64, std::byte{0xcc});
	auto cpu = mops::state();
	cpu.registers[destination_register] = destination;
	cpu.registers[source_register] = source;
	cpu.registers[count_register] = 64;
	const std::array instructions{
		mops::copy_instruction(false, mops::Stage::prologue, 0,
			destination_register, source_register, count_register),
		mops::copy_instruction(false, mops::Stage::main, 0,
			destination_register, source_register, count_register),
		mops::copy_instruction(false, mops::Stage::epilogue, 0,
			destination_register, source_register, count_register),
		0xd400'0001U};
	mops::test::require(mapping.execute(mops::pc, instructions, cpu) ==
		gadgets::Event::memory_fault);
	mops::test::require(cpu.pc == mops::pc + 8 &&
		cpu.registers[count_register] == 44 &&
		cpu.registers[source_register] == source + 20 &&
		cpu.registers[destination_register] == destination + 20 &&
		std::equal(mapping.at(source), mapping.at(source) + 20,
			mapping.at(destination)));
}

void register_and_reserved_matrices()
{
	constexpr std::uint64_t guest = 0x7600'0000ULL;
	mops::test::StaticMapping mapping{
		guest, 0x4d4f5003UL, "/tmp/uml-mops-cpyf-reg-XXXXXX"};
	mops::Memory memory;
	for (unsigned destination = 0; destination < 32; ++destination)
		for (unsigned source = 0; source < 32; ++source)
			for (unsigned count = 0; count < 32; ++count) {
				const auto encoded = mops::copy_instruction(false,
					mops::Stage::prologue, 0, destination, source, count);
				auto interpreted = mops::state();
				interpreted.registers[destination % 31] = mops::Memory::base;
				interpreted.registers[source % 31] = mops::Memory::base + 0x1000;
				interpreted.registers[count % 31] = 0;
				interpreted.pc = mops::pc;
				const bool valid = mops::valid_copy_registers(
					destination, source, count);
				const auto result = arm64::Dispatcher{}.execute(
					interpreted, memory, encoded);
				mops::test::require(result.reason == (valid ?
					arm64::StopReason::continued :
					arm64::StopReason::illegal_instruction));

				auto gadget = mops::state();
				gadget.registers[destination % 31] = guest;
				gadget.registers[source % 31] = guest + 0x1000;
				gadget.registers[count % 31] = 0;
				mops::test::require(mapping.execute(mops::pc, encoded, gadget) ==
					(valid ? gadgets::Event::system_call :
					 gadgets::Event::illegal_instruction));
			}

	for (unsigned options = 0; options < 16; ++options)
		for (const auto sz : {0x4000'0000U, 0x8000'0000U,
			0xc000'0000U}) {
			const auto reserved = mops::copy_instruction(false,
				mops::Stage::prologue, options, 3, 4, 5) | sz;
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
	mops::Memory memory;
	auto cpu = mops::state();
	cpu.registers[count_register] = 0x8000'0000'0000'0000ULL;
	cpu.pc = mops::pc;
	const auto encoded = mops::copy_instruction(false, mops::Stage::prologue,
		0, destination_register, source_register, count_register);
	mops::require_continued(arm64::Dispatcher{}.execute(
		cpu, memory, encoded), mops::pc, encoded);
	mops::test::require(cpu.registers[count_register] ==
		0x7fff'ffff'ffff'ffffULL && cpu.nzcv == 0x2000'0000U);

#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	std::set<std::uintptr_t> entries;
	for (const auto stage : {mops::Stage::prologue, mops::Stage::main,
		mops::Stage::epilogue}) {
		const auto built = gadgets::Builder{}.build(mops::pc,
			std::array{mops::copy_instruction(false, stage, 0,
				destination_register, source_register, count_register)});
		mops::test::require(built.complete &&
			built.program.instruction_count() == 1);
		entries.insert(built.program.words().front());
	}
	mops::test::require(entries.size() == 3);
#endif
}

} // namespace

int main()
{
	encoding_examples();
	interpreter_all_options();
	gadget_all_options_and_pages();
	interpreter_fault_restart_and_format_checks();
	gadget_fault_restart();
	register_and_reserved_matrices();
	saturation_and_specialization();
}
