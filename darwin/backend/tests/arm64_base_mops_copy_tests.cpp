// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_mops_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <utility>
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
	mops::test::require(mops::copy_instruction(true, mops::Stage::prologue,
		0, 0, 1, 2) == 0x1d01'0440U);
	mops::test::require(mops::copy_instruction(true, mops::Stage::main,
		0, 0, 1, 2) == 0x1d41'0440U);
	mops::test::require(mops::copy_instruction(true, mops::Stage::epilogue,
		0, 0, 1, 2) == 0x1d81'0440U);
	mops::test::require(mops::copy_instruction(true, mops::Stage::prologue,
		5, 3, 4, 5) == 0x1d04'54a3U); // CPYPWTWN
}

void interpreter_all_options_backward()
{
	constexpr std::uint64_t source = mops::Memory::base + 0x2000;
	constexpr std::uint64_t delta = 73;
	constexpr std::uint64_t destination = source + delta;
	constexpr std::size_t size = 257;
	constexpr std::size_t arena_size = size + delta;
	for (unsigned options = 0; options < 16; ++options) {
		mops::Memory memory;
		mops::fill_pattern(std::span{memory.at(source), arena_size},
			options + 17);
		std::vector<std::byte> expected(
			memory.at(source), memory.at(source) + arena_size);
		std::memmove(expected.data() + delta, expected.data(), size);
		auto cpu = mops::state();
		cpu.registers[destination_register] = destination;
		cpu.registers[source_register] = source;
		cpu.registers[count_register] = size;
		cpu.pc = mops::pc;

		const auto prologue = mops::copy_instruction(true,
			mops::Stage::prologue, options, destination_register,
			source_register, count_register);
		mops::require_continued(arm64::Dispatcher{}.execute(
			cpu, memory, prologue), mops::pc, prologue);
		mops::test::require(cpu.pc == mops::pc + 4 &&
			cpu.nzcv == 0xa000'0000U &&
			cpu.registers[count_register] == size &&
			cpu.registers[source_register] == source + size &&
			cpu.registers[destination_register] == destination + size &&
			memory.reads == 0 && memory.writes == 0);

		const auto main = mops::copy_instruction(true, mops::Stage::main,
			options, destination_register, source_register, count_register);
		mops::require_continued(arm64::Dispatcher{}.execute(
			cpu, memory, main), mops::pc + 4, main);
		mops::test::require(cpu.pc == mops::pc + 8 &&
			cpu.registers[count_register] == size &&
			memory.reads == 0 && memory.writes == 0);

		const auto epilogue = mops::copy_instruction(true,
			mops::Stage::epilogue, options, destination_register,
			source_register, count_register);
		mops::require_continued(arm64::Dispatcher{}.execute(
			cpu, memory, epilogue), mops::pc + 8, epilogue);
		mops::test::require(cpu.pc == mops::pc + 12 &&
			cpu.registers[count_register] == 0 &&
			cpu.registers[source_register] == source &&
			cpu.registers[destination_register] == destination &&
			std::equal(expected.begin(), expected.end(), memory.at(source)) &&
			memory.reads == size && memory.writes == size);
	}
}

void gadget_all_options_overlap_and_pages()
{
	constexpr std::uint64_t guest = 0x7700'0000ULL;
	mops::test::StaticMapping mapping{
		guest, 0x4d4f5101UL, "/tmp/uml-mops-cpy-XXXXXX"};
	constexpr std::size_t size = 5000;
	constexpr std::uint64_t delta = 173;
	const auto lower = guest + 5 * mops::test::StaticMapping::page_size + 211;
	for (unsigned options = 0; options < 16; ++options) {
		const bool backward = (options & 1U) == 0;
		const auto source = backward ? lower : lower + delta;
		const auto destination = backward ? lower + delta : lower;
		mops::fill_pattern(std::span{mapping.at(lower), size + delta},
			options + 23);
		std::vector<std::byte> expected(
			mapping.at(lower), mapping.at(lower) + size + delta);
		std::memmove(expected.data() + (destination - lower),
			expected.data() + (source - lower), size);
		auto cpu = mops::state();
		cpu.registers[destination_register] = destination;
		cpu.registers[source_register] = source;
		cpu.registers[count_register] = size;
		const std::array instructions{
			mops::copy_instruction(true, mops::Stage::prologue, options,
				destination_register, source_register, count_register),
			mops::copy_instruction(true, mops::Stage::main, options,
				destination_register, source_register, count_register),
			mops::copy_instruction(true, mops::Stage::epilogue, options,
				destination_register, source_register, count_register),
			0xd400'0001U};
		mops::test::require(mapping.execute(mops::pc, instructions, cpu) ==
			gadgets::Event::system_call);
		mops::test::require(std::equal(expected.begin(), expected.end(),
			mapping.at(lower)) && cpu.pc == mops::pc + 16 &&
			cpu.nzcv == (backward ? 0xa000'0000U : 0x2000'0000U) &&
			cpu.registers[count_register] == 0 &&
			cpu.registers[source_register] ==
				(backward ? source : source + size) &&
			cpu.registers[destination_register] ==
				(backward ? destination : destination + size));
	}
}

void direction_selection()
{
	struct DirectionCase final {
		std::uint64_t source;
		std::uint64_t destination;
		std::uint64_t size;
		bool backward;
	};
	constexpr std::array cases{
		DirectionCase{0x1000, 0x1008, 32, true},
		DirectionCase{0x1008, 0x1000, 32, false},
		DirectionCase{0x1000, 0x2000, 32, false},
		DirectionCase{0x2000, 0x1000, 32, false}};
	mops::Memory memory;
	for (const auto &item : cases) {
		auto cpu = mops::state();
		cpu.registers[destination_register] = item.destination;
		cpu.registers[source_register] = item.source;
		cpu.registers[count_register] = item.size;
		cpu.pc = mops::pc;
		const auto encoded = mops::copy_instruction(true,
			mops::Stage::prologue, 0, destination_register,
			source_register, count_register);
		mops::require_continued(arm64::Dispatcher{}.execute(
			cpu, memory, encoded), mops::pc, encoded);
		mops::test::require(cpu.nzcv ==
			(item.backward ? 0xa000'0000U : 0x2000'0000U) &&
			cpu.registers[source_register] == item.source +
				(item.backward ? item.size : 0) &&
			cpu.registers[destination_register] == item.destination +
				(item.backward ? item.size : 0));
	}
}

void interpreter_backward_fault_restart_and_format_checks()
{
	constexpr std::uint64_t source = mops::Memory::base + 0x3000;
	constexpr std::uint64_t destination = source + 8;
	constexpr std::uint64_t size = 32;
	for (const bool write_fault : {false, true}) {
		mops::Memory memory;
		mops::fill_pattern(std::span{memory.at(source), size + 8}, 31);
		std::vector<std::byte> original(
			memory.at(source), memory.at(source) + size);
		const unsigned completed = write_fault ? 11U : 7U;
		const auto offset = size - completed - 1;
		if (write_fault)
			memory.fault_write = destination + offset;
		else
			memory.fault_read = source + offset;
		auto cpu = mops::state();
		cpu.registers[destination_register] = destination;
		cpu.registers[source_register] = source;
		cpu.registers[count_register] = size;
		cpu.pc = mops::pc;
		const auto prologue = mops::copy_instruction(true,
			mops::Stage::prologue, 0, destination_register,
			source_register, count_register);
		mops::require_continued(arm64::Dispatcher{}.execute(
			cpu, memory, prologue), mops::pc, prologue);
		const auto epilogue = mops::copy_instruction(true,
			mops::Stage::epilogue, 0, destination_register,
			source_register, count_register);
		const auto fault = arm64::Dispatcher{}.execute(cpu, memory, epilogue);
		mops::test::require(fault.reason == arm64::StopReason::memory_fault &&
			fault.address == (write_fault ? destination : source) + offset &&
			fault.write == write_fault && cpu.pc == mops::pc + 4 &&
			cpu.registers[count_register] == size - completed &&
			cpu.registers[source_register] == source + size - completed &&
			cpu.registers[destination_register] ==
				destination + size - completed);
		for (unsigned index = 0; index < completed; ++index) {
			const auto copied = size - 1 - index;
			mops::test::require(memory.at(destination)[copied] ==
				original[copied]);
		}
	}

	mops::Memory memory;
	for (const auto malformed : {std::pair{std::uint64_t{8}, 0U},
		std::pair{0x0080'0000'0000'0000ULL, 0x2000'0000U}}) {
		auto cpu = mops::state();
		cpu.registers[destination_register] = destination;
		cpu.registers[source_register] = source;
		cpu.registers[count_register] = malformed.first;
		cpu.nzcv = malformed.second;
		cpu.pc = mops::pc;
		const auto before = cpu;
		const auto encoded = mops::copy_instruction(true,
			mops::Stage::epilogue, 0, destination_register,
			source_register, count_register);
		mops::test::require(arm64::Dispatcher{}.execute(
			cpu, memory, encoded).reason ==
			arm64::StopReason::illegal_instruction);
		mops::require_state(cpu, before, mops::pc);
		mops::test::require(memory.reads == 0 && memory.writes == 0);
	}
}

void gadget_backward_fault_restart()
{
	constexpr std::uint64_t guest = 0x7800'0000ULL;
	mops::test::StaticMapping mapping{
		guest, 0x4d4f5102UL, "/tmp/uml-mops-cpy-fault-XXXXXX"};
	const auto source = guest - 20;
	const auto destination = guest;
	constexpr std::uint64_t size = 64;
	mops::fill_pattern(std::span{mapping.at(guest), 64}, 37);
	std::vector<std::byte> original(mapping.at(guest), mapping.at(guest) + 44);
	auto cpu = mops::state();
	cpu.registers[destination_register] = destination;
	cpu.registers[source_register] = source;
	cpu.registers[count_register] = size;
	const std::array instructions{
		mops::copy_instruction(true, mops::Stage::prologue, 0,
			destination_register, source_register, count_register),
		mops::copy_instruction(true, mops::Stage::epilogue, 0,
			destination_register, source_register, count_register),
		0xd400'0001U};
	mops::test::require(mapping.execute(mops::pc, instructions, cpu) ==
		gadgets::Event::memory_fault);
	mops::test::require(cpu.pc == mops::pc + 4 &&
		cpu.nzcv == 0xa000'0000U &&
		cpu.registers[count_register] == 20 &&
		cpu.registers[source_register] == guest &&
		cpu.registers[destination_register] == guest + 20 &&
		std::equal(original.begin(), original.end(), mapping.at(guest + 20)));
}

void invalid_reserved_saturation_and_specialization()
{
	constexpr std::array invalid_registers{
		std::array{31U, 4U, 5U}, std::array{3U, 31U, 5U},
		std::array{3U, 4U, 31U}, std::array{3U, 3U, 5U},
		std::array{3U, 4U, 3U}, std::array{3U, 4U, 4U}};
	constexpr std::uint64_t guest = 0x7900'0000ULL;
	mops::test::StaticMapping mapping{
		guest, 0x4d4f5103UL, "/tmp/uml-mops-cpy-invalid-XXXXXX"};
	mops::Memory memory;
	for (const auto &registers : invalid_registers) {
		const auto encoded = mops::copy_instruction(true,
			mops::Stage::prologue, 0, registers[0], registers[1],
			registers[2]);
		auto cpu = mops::state();
		cpu.pc = mops::pc;
		mops::test::require(arm64::Dispatcher{}.execute(
			cpu, memory, encoded).reason ==
			arm64::StopReason::illegal_instruction);
		mops::test::require(mapping.execute(mops::pc, encoded, cpu) ==
			gadgets::Event::illegal_instruction);
	}
	for (unsigned options = 0; options < 16; ++options)
		for (const auto sz : {0x4000'0000U, 0x8000'0000U,
			0xc000'0000U}) {
			const auto reserved = mops::copy_instruction(true,
				mops::Stage::prologue, options, 3, 4, 5) | sz;
			auto cpu = mops::state();
			cpu.pc = mops::pc;
			mops::test::require(arm64::Dispatcher{}.execute(
				cpu, memory, reserved).reason ==
				arm64::StopReason::illegal_instruction);
			mops::test::require(mapping.execute(mops::pc, reserved, cpu) ==
				gadgets::Event::illegal_instruction);
		}

	auto interpreted = mops::state();
	interpreted.registers[count_register] = ~std::uint64_t{};
	interpreted.pc = mops::pc;
	const auto encoded = mops::copy_instruction(true, mops::Stage::prologue,
		0, destination_register, source_register, count_register);
	mops::require_continued(arm64::Dispatcher{}.execute(
		interpreted, memory, encoded), mops::pc, encoded);
	mops::test::require(interpreted.registers[count_register] ==
		0x007f'ffff'ffff'ffffULL);
	auto gadget = mops::state();
	gadget.registers[count_register] = ~std::uint64_t{};
	mops::test::require(mapping.execute(mops::pc, encoded, gadget) ==
		gadgets::Event::system_call);
	mops::test::require(gadget.registers[count_register] ==
		0x007f'ffff'ffff'ffffULL);

#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	std::set<std::uintptr_t> entries;
	for (const auto stage : {mops::Stage::prologue, mops::Stage::main,
		mops::Stage::epilogue}) {
		const auto built = gadgets::Builder{}.build(mops::pc,
			std::array{mops::copy_instruction(true, stage, 0,
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
	interpreter_all_options_backward();
	gadget_all_options_overlap_and_pages();
	direction_selection();
	interpreter_backward_fault_restart_and_format_checks();
	gadget_backward_fault_restart();
	invalid_reserved_saturation_and_specialization();
}
