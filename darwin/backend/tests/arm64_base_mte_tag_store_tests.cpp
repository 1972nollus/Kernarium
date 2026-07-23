// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_mte_memory_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>

namespace mte = mte_memory_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::array operations{
	mte::Operation::store_tag,
	mte::Operation::store_two_tags,
};
constexpr std::array modes{
	mte::Mode::post_index,
	mte::Mode::signed_offset,
	mte::Mode::pre_index,
};

[[nodiscard]] constexpr bool two_tags(mte::Operation operation) noexcept
{
	return operation == mte::Operation::store_two_tags;
}

void encoding_examples()
{
	mte::test::require(mte::tag_memory(mte::Operation::store_tag,
		mte::Mode::signed_offset, 0, 6, 5) == 0xd920'08c5U);
	mte::test::require(mte::tag_memory(mte::Operation::store_tag,
		mte::Mode::post_index, 1, 31, 31) == 0xd920'17ffU);
	mte::test::require(mte::tag_memory(mte::Operation::store_tag,
		mte::Mode::pre_index, 0x1ff, 8, 7) == 0xd93f'fd07U);
	mte::test::require(mte::tag_memory(mte::Operation::store_two_tags,
		mte::Mode::signed_offset, 0, 10, 9) == 0xd9a0'0949U);
	mte::test::require(mte::tag_memory(mte::Operation::store_two_tags,
		mte::Mode::post_index, 2, 11, 31) == 0xd9a0'257fU);
}

void interpreter_all_registers_modes_and_operations()
{
	mte::Memory memory;
	const auto target_granule = mte::Memory::base + 2 * 4096 + 0x240;
	for (const auto operation : operations)
		for (const auto mode : modes)
			for (unsigned base = 0; base < 32; ++base)
				for (unsigned transfer = 0; transfer < 32; ++transfer) {
					const unsigned imm9 = (base * 73U + transfer * 19U +
						static_cast<unsigned>(mode) * 7U) & 0x1ffU;
					const auto target = target_granule +
						(base == 31 ? 0U : (base + transfer) & 15U);
					const auto base_address =
						mode == mte::Mode::post_index ? target :
						target - mte::offset9(imm9);
					auto cpu = mte::state();
					mte::set_register(cpu, base,
						base_address |
						(static_cast<std::uint64_t>(
							(base * 5U + 1U) & 15U) << 56), true);
					if (transfer != base)
						mte::set_register(cpu, transfer,
							(static_cast<std::uint64_t>(
								(transfer * 11U + 7U) & 15U) << 56) |
							0x0012'3456'789a'bcdeULL, true);
					cpu.pc = mte::pc;
					auto expected = cpu;
					const auto tag = static_cast<std::uint8_t>(
						mte::get_register(cpu, transfer, true) >> 56) & 15U;
					if (mode == mte::Mode::post_index)
						mte::set_register(expected, base,
							mte::get_register(cpu, base, true) +
							mte::offset9(imm9), true);
					else if (mode == mte::Mode::pre_index)
						mte::set_register(expected, base,
							mte::get_register(cpu, base, true) +
							mte::offset9(imm9), true);
					const auto bytes_before = memory.bytes;
					memory.set_tag(target, 1);
					memory.set_tag(target + 16, 2);
					memory.reset_counts();
					const auto instruction = mte::tag_memory(
						operation, mode, imm9, base, transfer);
					const auto result = arm64::Dispatcher{}.execute(
						cpu, memory, instruction);
					mte::require_continued(result, instruction);
					mte::test::require(memory.bytes == bytes_before &&
						memory.reads == 0 && memory.writes == 0 &&
						memory.tag_reads == 0 &&
						memory.tag_writes == (two_tags(operation) ? 2U : 1U) &&
						memory.tag(target) == tag &&
						(!two_tags(operation) || memory.tag(target + 16) == tag));
					mte::require_state(cpu, expected, mte::pc + 4);
				}
}

void gadget_all_registers_modes_and_operations()
{
	constexpr std::uint64_t guest = 0x7a00'0000ULL;
	constexpr unsigned long generation = 0x6d7b01UL;
	mte::test::StaticMapping mapping{
		guest, generation, "/tmp/uml-mte-tag-store-XXXXXX"};
	const auto target_granule = guest + 2 * 4096 + 0x240;
	for (const auto operation : operations)
		for (const auto mode : modes)
			for (unsigned base = 0; base < 32; ++base)
				for (unsigned transfer = 0; transfer < 32; ++transfer) {
					const unsigned imm9 = (base * 73U + transfer * 19U +
						static_cast<unsigned>(mode) * 7U) & 0x1ffU;
					const auto target = target_granule +
						(base == 31 ? 0U : (base + transfer) & 15U);
					const auto base_address =
						mode == mte::Mode::post_index ? target :
						target - mte::offset9(imm9);
					auto cpu = mte::state();
					mte::set_register(cpu, base,
						base_address |
						(static_cast<std::uint64_t>(
							(base * 5U + 1U) & 15U) << 56), true);
					if (transfer != base)
						mte::set_register(cpu, transfer,
							(static_cast<std::uint64_t>(
								(transfer * 11U + 7U) & 15U) << 56) |
							0x0012'3456'789a'bcdeULL, true);
					auto expected = cpu;
					const auto tag = static_cast<std::uint8_t>(
						mte::get_register(cpu, transfer, true) >> 56) & 15U;
					if (mode == mte::Mode::post_index ||
					    mode == mte::Mode::pre_index)
						mte::set_register(expected, base,
							mte::get_register(cpu, base, true) +
							mte::offset9(imm9), true);
					std::array<std::byte, 32> before{};
					std::copy_n(mapping.at(target_granule), before.size(),
						before.begin());
					mte::static_store_tag(generation, target, 1);
					mte::static_store_tag(generation, target + 16, 2);
					const auto event = mapping.execute(mte::pc,
						mte::tag_memory(operation, mode, imm9, base, transfer),
						cpu);
					mte::test::require(event == gadgets::Event::system_call &&
						std::equal(before.begin(), before.end(),
							mapping.at(target_granule)) &&
						mte::static_load_tag(generation, target) == tag &&
						(!two_tags(operation) ||
						 mte::static_load_tag(generation, target + 16) == tag));
					mte::require_state(cpu, expected, mte::pc + 8);
				}
}

void every_immediate_and_unaligned_granule_selection()
{
	mte::Memory memory;
	const auto target = mte::Memory::base + 2 * 4096 + 0x500;
	for (const auto operation : operations)
		for (const auto mode : modes)
			for (unsigned imm9 = 0; imm9 < 512; ++imm9) {
				auto cpu = mte::state();
				cpu.registers[1] = (mode == mte::Mode::post_index ?
					target + 15 : target + 15 - mte::offset9(imm9));
				cpu.registers[2] =
					(static_cast<std::uint64_t>(imm9 & 15U) << 56);
				cpu.pc = mte::pc;
				const auto instruction = mte::tag_memory(
					operation, mode, imm9, 1, 2);
				const auto result = arm64::Dispatcher{}.execute(
					cpu, memory, instruction);
				mte::require_continued(result, instruction);
				mte::test::require(memory.tag(target) == (imm9 & 15U));
			}
}

void fault_ordering_and_writeback_safety()
{
	const auto address = mte::Memory::base + 2 * 4096 + 0x300;
	for (const auto operation : operations)
		for (const auto mode : modes) {
			mte::Memory memory;
			memory.fault_tag_write = address;
			auto cpu = mte::state();
			cpu.registers[1] = mode == mte::Mode::post_index ? address :
				address - 16;
			cpu.registers[2] = 0x0d00'0000'0000'0000ULL;
			cpu.pc = mte::pc;
			const auto before = cpu;
			const unsigned imm9 = mode == mte::Mode::post_index ? 1U : 1U;
			const auto instruction = mte::tag_memory(
				operation, mode, imm9, 1, 2);
			const auto result = arm64::Dispatcher{}.execute(
				cpu, memory, instruction);
			mte::test::require(result.reason == arm64::StopReason::memory_fault &&
				result.address == address && result.write &&
				result.instructions == 0 && memory.tag_writes == 1 &&
				memory.reads == 0 && memory.writes == 0);
			mte::require_state(cpu, before, mte::pc);
		}

	mte::Memory memory;
	memory.set_tag(address, 1);
	memory.set_tag(address + 16, 2);
	memory.fault_tag_write = address + 16;
	auto cpu = mte::state();
	cpu.registers[1] = address;
	cpu.registers[2] = 0x0e00'0000'0000'0000ULL;
	cpu.pc = mte::pc;
	const auto before = cpu;
	const auto instruction = mte::tag_memory(mte::Operation::store_two_tags,
		mte::Mode::post_index, 1, 1, 2);
	const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
	mte::test::require(result.reason == arm64::StopReason::memory_fault &&
		result.address == address + 16 && result.write &&
		memory.tag_writes == 2 && memory.tag(address) == 14 &&
		memory.tag(address + 16) == 2);
	mte::require_state(cpu, before, mte::pc);

	memory.reset_counts();
	auto sp = mte::state();
	sp.sp = address + 1;
	sp.pc = mte::pc;
	const auto before_sp = sp;
	const auto sp_instruction = mte::tag_memory(mte::Operation::store_tag,
		mte::Mode::signed_offset, 0, 31, 2);
	const auto sp_result = arm64::Dispatcher{}.execute(
		sp, memory, sp_instruction);
	mte::test::require(sp_result.reason == arm64::StopReason::memory_fault &&
		sp_result.address == address + 1 && sp_result.write &&
		memory.tag_writes == 0 && memory.writes == 0);
	mte::require_state(sp, before_sp, mte::pc);
}

void gadget_fault_safety()
{
	constexpr std::uint64_t guest = 0x7b00'0000ULL;
	mte::test::StaticMapping mapping{
		guest, 0x6d7b02UL, "/tmp/uml-mte-tag-store-fault-XXXXXX"};
	for (const auto operation : operations)
		for (const auto mode : modes) {
			auto cpu = mte::state();
			cpu.registers[1] = guest + mte::test::StaticMapping::length;
			cpu.registers[2] = 0x0c00'0000'0000'0000ULL;
			const auto before = cpu;
			mte::test::require(mapping.execute(mte::pc,
				mte::tag_memory(operation, mode, 0, 1, 2), cpu) ==
				gadgets::Event::memory_fault);
			mte::require_state(cpu, before, mte::pc);
		}
}

void production_specialization()
{
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
	return;
#else
	std::set<std::uintptr_t> entries;
	for (const auto operation : operations)
		for (const auto mode : modes)
			for (const unsigned base : {1U, 31U})
				for (const unsigned source : {2U, 31U}) {
					const auto built = gadgets::Builder{}.build(mte::pc,
						std::array{mte::tag_memory(
							operation, mode, 0, base, source)});
					mte::test::require(built.complete &&
						built.program.instruction_count() == 1);
					entries.insert(built.program.words().front());
				}
	/* Source register/SP is encoded as a direct operand offset, not a helper
	 * branch; operation, mode, and base-SP alignment are specialized. */
	mte::test::require(entries.size() == 12);
#endif
}

} // namespace

int main()
{
	encoding_examples();
	interpreter_all_registers_modes_and_operations();
	gadget_all_registers_modes_and_operations();
	every_immediate_and_unaligned_granule_selection();
	fault_ordering_and_writeback_safety();
	gadget_fault_safety();
	production_specialization();
}
