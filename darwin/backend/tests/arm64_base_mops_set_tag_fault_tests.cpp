// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_mops_test_support.hpp"
#include "arm64_mte_memory_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>

namespace mops = mops_test;
namespace mte = mte_memory_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr unsigned destination_register = 3;
constexpr unsigned count_register = 4;
constexpr unsigned source_register = 5;

[[nodiscard]] constexpr std::uint32_t epilogue(unsigned options = 0)
{
	return mops::set_tag_instruction(mops::Stage::epilogue, options,
		destination_register, count_register, source_register);
}

[[nodiscard]] arm64::CpuState epilogue_state(
	std::uint64_t address, std::uint64_t count)
{
	auto cpu = mops::state();
	cpu.registers[destination_register] = address;
	cpu.registers[count_register] = count;
	cpu.registers[source_register] = 0xa5;
	cpu.nzcv = 0x2000'0000U;
	cpu.pc = mops::pc;
	return cpu;
}

void interpreter_data_fault_then_tag_progress()
{
	constexpr auto address = mte::Memory::base + 4096;
	mte::Memory memory;
	std::fill_n(memory.at(address), 32, std::byte{0xcc});
	memory.fault_write = address + 5;
	auto cpu = epilogue_state(address | 0x0b00'0000'0000'0000ULL, 32);
	const auto result = arm64::Dispatcher{}.execute(cpu, memory, epilogue());
	mops::test::require(result.reason == arm64::StopReason::memory_fault &&
		result.address == address + 5 && result.write &&
		result.instructions == 0 && cpu.pc == mops::pc &&
		cpu.registers[count_register] == 16 &&
		cpu.registers[destination_register] ==
			(address | 0x0b00'0000'0000'0000ULL) + 16 &&
		memory.writes == 6 && memory.tag_writes == 1 &&
		memory.tag(address) == 11 &&
		std::all_of(memory.at(address), memory.at(address) + 5,
			[](std::byte value) { return value == std::byte{0xa5}; }) &&
		std::all_of(memory.at(address) + 5, memory.at(address) + 32,
			[](std::byte value) { return value == std::byte{0xcc}; }));
}

void interpreter_tag_fault_retains_data_without_progress()
{
	constexpr auto address = mte::Memory::base + 4096;
	mte::Memory memory;
	std::fill_n(memory.at(address), 16, std::byte{0xcc});
	memory.set_tag(address, 3);
	memory.fault_tag_write = address;
	auto cpu = epilogue_state(address | 0x0d00'0000'0000'0000ULL, 16);
	const auto before = cpu;
	const auto result = arm64::Dispatcher{}.execute(cpu, memory, epilogue(3));
	mops::test::require(result.reason == arm64::StopReason::memory_fault &&
		result.address == address && result.write &&
		memory.writes == 16 && memory.tag_writes == 1 &&
		memory.tag(address) == 3 &&
		std::all_of(memory.at(address), memory.at(address) + 16,
			[](std::byte value) { return value == std::byte{0xa5}; }));
	mops::require_state(cpu, before, mops::pc);
}

void interpreter_later_tag_fault_commits_prior_granules()
{
	constexpr auto address = mte::Memory::base + 4096;
	mte::Memory memory;
	std::fill_n(memory.at(address), 32, std::byte{0xcc});
	memory.set_tag(address, 1);
	memory.set_tag(address + 16, 2);
	memory.fault_tag_write = address + 16;
	auto cpu = epilogue_state(address | 0x0900'0000'0000'0000ULL, 32);
	const auto result = arm64::Dispatcher{}.execute(cpu, memory, epilogue(2));
	mops::test::require(result.reason == arm64::StopReason::memory_fault &&
		result.address == address + 16 && result.write &&
		cpu.registers[count_register] == 16 &&
		cpu.registers[destination_register] ==
			(address | 0x0900'0000'0000'0000ULL) + 16 &&
		memory.writes == 32 && memory.tag_writes == 2 &&
		memory.tag(address) == 9 && memory.tag(address + 16) == 2 &&
		std::all_of(memory.at(address), memory.at(address) + 32,
			[](std::byte value) { return value == std::byte{0xa5}; }));
}

void strict_epilogue_checks_precede_memory()
{
	constexpr auto address = mte::Memory::base + 4096;
	for (const auto [destination, count, nzcv, reason] : {
	     std::tuple{std::uint64_t{address}, std::uint64_t{16}, 0U,
		arm64::StopReason::illegal_instruction},
	     std::tuple{std::uint64_t{address}, std::uint64_t{16}, 0xa000'0000U,
		arm64::StopReason::illegal_instruction},
	     std::tuple{std::uint64_t{address}, 0x8000'0000'0000'0000ULL,
		0x2000'0000U, arm64::StopReason::illegal_instruction},
	     std::tuple{std::uint64_t{address + 1}, std::uint64_t{16},
		0x2000'0000U, arm64::StopReason::memory_fault},
	     std::tuple{std::uint64_t{address}, std::uint64_t{17},
		0x2000'0000U, arm64::StopReason::memory_fault}}) {
		mte::Memory memory;
		auto cpu = epilogue_state(destination, count);
		cpu.nzcv = nzcv;
		const auto before = cpu;
		const auto result = arm64::Dispatcher{}.execute(cpu, memory, epilogue());
		mops::test::require(result.reason == reason &&
			result.instructions == 0 && memory.reads == 0 &&
			memory.writes == 0 && memory.tag_reads == 0 &&
			memory.tag_writes == 0);
		mops::require_state(cpu, before, mops::pc);
	}

	/* With zero bytes remaining, this implementation elects not to check
	 * option format or alignment, as allowed by both zero-size hooks. */
	mte::Memory memory;
	auto zero = epilogue_state(address + 1, 0);
	zero.nzcv = 0xa000'0000U;
	const auto instruction = epilogue(1);
	mops::require_continued(arm64::Dispatcher{}.execute(
		zero, memory, instruction), mops::pc, instruction);
	mops::test::require(zero.registers[count_register] == 0 &&
		zero.registers[destination_register] == address + 1 &&
		memory.writes == 0 && memory.tag_writes == 0);
}

void gadget_page_fault_restart_and_no_partial_block()
{
	constexpr std::uint64_t guest = 0x8100'0000ULL;
	constexpr unsigned long generation = 0x4d4f5311UL;
	mops::test::StaticMapping mapping{
		guest, generation, "/tmp/uml-mops-setg-fault-XXXXXX"};
	const auto address = guest + mops::test::StaticMapping::length - 32;
	std::fill_n(mapping.at(address), 32, std::byte{0xcc});
	auto cpu = epilogue_state(address | 0x0e00'0000'0000'0000ULL, 64);
	const auto before_source = cpu.registers[source_register];
	mops::test::require(mapping.execute(mops::pc, epilogue(), cpu) ==
		gadgets::Event::memory_fault && cpu.pc == mops::pc &&
		cpu.registers[count_register] == 32 &&
		cpu.registers[destination_register] ==
			(address | 0x0e00'0000'0000'0000ULL) + 32 &&
		cpu.registers[source_register] == before_source &&
		std::all_of(mapping.at(address), mapping.at(address) + 32,
			[](std::byte value) { return value == std::byte{0xa5}; }) &&
		mte::static_load_tag(generation, address) == 14 &&
		mte::static_load_tag(generation, address + 16) == 14);
}

void gadget_alignment_fault_preserves_state()
{
	constexpr std::uint64_t guest = 0x8200'0000ULL;
	mops::test::StaticMapping mapping{
		guest, 0x4d4f5312UL, "/tmp/uml-mops-setg-align-XXXXXX"};
	for (const auto stage : {mops::Stage::prologue, mops::Stage::epilogue}) {
		auto cpu = mops::state();
		cpu.registers[destination_register] = guest + 1;
		cpu.registers[count_register] = 16;
		cpu.registers[source_register] = 0x5a;
		cpu.nzcv = 0x2000'0000U;
		const auto before = cpu;
		const auto instruction = mops::set_tag_instruction(stage, 0,
			destination_register, count_register, source_register);
		mops::test::require(mapping.execute(mops::pc, instruction, cpu) ==
			gadgets::Event::memory_fault);
		mops::require_state(cpu, before, mops::pc);
	}
}

} // namespace

int main()
{
	interpreter_data_fault_then_tag_progress();
	interpreter_tag_fault_retains_data_without_progress();
	interpreter_later_tag_fault_commits_prior_granules();
	strict_epilogue_checks_precede_memory();
	gadget_page_fault_restart_and_no_partial_block();
	gadget_alignment_fault_preserves_state();
}
