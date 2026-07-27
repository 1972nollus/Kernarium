// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_mte_memory_test_support.hpp"
#include "uml_darwin_guest.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <sys/mman.h>
#include <unistd.h>

namespace mte = mte_memory_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t gadget_guest = 0x7500'0000ULL;
constexpr unsigned long gadget_generation = 0x6d7a01UL;

void encoding_examples()
{
	mte::test::require(mte::load_tag(0, 1, 0) == 0xd960'0020U);
	mte::test::require(mte::load_tag(0x100, 31, 2) == 0xd970'03e2U);
	mte::test::require(mte::load_tag(0x1ff, 30, 31) == 0xd97f'f3dfU);
}

void interpreter_all_registers()
{
	mte::Memory memory;
	const auto target = mte::Memory::base + 2 * 4096 + 0x230;
	for (unsigned base = 0; base < 32; ++base)
		for (unsigned destination = 0; destination < 32; ++destination) {
			const unsigned imm9 = (base * 37U + destination * 11U) & 0x1ffU;
			const auto base_address = target - mte::offset9(imm9) |
				(static_cast<std::uint64_t>((base + 5U) & 15U) << 56);
			auto cpu = mte::state();
			mte::set_register(cpu, base, base_address, true);
			cpu.pc = mte::pc;
			auto expected = cpu;
			const auto tag = static_cast<std::uint8_t>(
				(base * 3U + destination * 5U) & 15U);
			memory.set_tag(target, tag);
			if (destination != 31)
				expected.registers[destination] =
					(expected.registers[destination] &
					 ~0x0f00'0000'0000'0000ULL) |
					(static_cast<std::uint64_t>(tag) << 56);
			memory.reset_counts();
			const auto instruction = mte::load_tag(imm9, base, destination);
			const auto result = arm64::Dispatcher{}.execute(
				cpu, memory, instruction);
			mte::require_continued(result, instruction);
			mte::test::require(memory.reads == 0 && memory.writes == 0 &&
				memory.tag_reads == 1 && memory.tag_writes == 0 &&
				memory.last_tag_address == target);
			mte::require_state(cpu, expected, mte::pc + 4);
		}
}

void gadget_all_registers()
{
	mte::test::StaticMapping mapping{gadget_guest, gadget_generation,
		"/tmp/uml-mte-ldg-XXXXXX"};
	const auto target = gadget_guest + 2 * 4096 + 0x230;
	for (unsigned base = 0; base < 32; ++base)
		for (unsigned destination = 0; destination < 32; ++destination) {
			const unsigned imm9 = (base * 37U + destination * 11U) & 0x1ffU;
			const auto base_address = target - mte::offset9(imm9) |
				(static_cast<std::uint64_t>((base + 5U) & 15U) << 56);
			auto cpu = mte::state();
			mte::set_register(cpu, base, base_address, true);
			auto expected = cpu;
			const auto tag = static_cast<std::uint8_t>(
				(base * 3U + destination * 5U) & 15U);
			mte::static_store_tag(gadget_generation, target, tag);
			if (destination != 31)
				expected.registers[destination] =
					(expected.registers[destination] &
					 ~0x0f00'0000'0000'0000ULL) |
					(static_cast<std::uint64_t>(tag) << 56);
			const auto event = mapping.execute(mte::pc,
				mte::load_tag(imm9, base, destination), cpu);
			mte::test::require(event == gadgets::Event::system_call);
			mte::require_state(cpu, expected, mte::pc + 8);
		}
}

void every_immediate_and_granule_alignment()
{
	mte::Memory memory;
	const auto target = mte::Memory::base + 2 * 4096 + 0x400;
	for (unsigned imm9 = 0; imm9 < 512; ++imm9) {
		auto cpu = mte::state();
		cpu.registers[1] = target - mte::offset9(imm9) + 13;
		cpu.registers[2] = 0xabcd'0123'4567'89efULL;
		cpu.pc = mte::pc;
		memory.set_tag(target, static_cast<std::uint8_t>(imm9));
		const auto result = arm64::Dispatcher{}.execute(
			cpu, memory, mte::load_tag(imm9, 1, 2));
		mte::require_continued(result, mte::load_tag(imm9, 1, 2));
		mte::test::require(memory.last_tag_address == target &&
			((cpu.registers[2] >> 56) & 15U) == (imm9 & 15U) &&
			(cpu.registers[2] & ~0x0f00'0000'0000'0000ULL) ==
				(0xabcd'0123'4567'89efULL &
				 ~0x0f00'0000'0000'0000ULL));
	}
}

void fault_safety()
{
	mte::Memory memory;
	const auto address = mte::Memory::base + 0x300;
	memory.fault_tag_read = address;
	auto cpu = mte::state();
	cpu.registers[1] = address + 7;
	cpu.pc = mte::pc;
	const auto before = cpu;
	const auto instruction = mte::load_tag(0, 1, 2);
	const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
	mte::test::require(result.reason == arm64::StopReason::memory_fault &&
		result.instructions == 0 && result.address == address &&
		result.instruction == instruction && !result.write &&
		memory.tag_reads == 1 && memory.reads == 0 && memory.writes == 0);
	mte::require_state(cpu, before, mte::pc);

	memory.reset_counts();
	auto misaligned_sp = mte::state();
	misaligned_sp.sp = address + 1;
	misaligned_sp.pc = mte::pc;
	const auto before_sp = misaligned_sp;
	const auto sp_instruction = mte::load_tag(0, 31, 3);
	const auto sp_result = arm64::Dispatcher{}.execute(
		misaligned_sp, memory, sp_instruction);
	mte::test::require(sp_result.reason == arm64::StopReason::memory_fault &&
		sp_result.address == address + 1 && !sp_result.write &&
		memory.tag_reads == 0 && memory.reads == 0 && memory.writes == 0);
	mte::require_state(misaligned_sp, before_sp, mte::pc);

	mte::test::StaticMapping mapping{0x7600'0000ULL, 0x6d7a02UL,
		"/tmp/uml-mte-ldg-fault-XXXXXX"};
	auto gadget = mte::state();
	gadget.sp = mapping.guest() + mte::test::StaticMapping::length;
	const auto before_gadget = gadget;
	mte::test::require(mapping.execute(mte::pc,
		mte::load_tag(0, 31, 4), gadget) == gadgets::Event::memory_fault);
	mte::require_state(gadget, before_gadget, mte::pc);
}

void privileged_and_reserved_encodings_are_early_illegal()
{
	mte::Memory memory;
	memory.fault_tag_read = mte::Memory::base;
	mte::test::StaticMapping mapping{0x7700'0000ULL, 0x6d7a03UL,
		"/tmp/uml-mte-ldg-illegal-XXXXXX"};
	for (const auto base_encoding : {0xd9e0'0000U, 0xd9a0'0000U,
	     0xd920'0000U})
		for (unsigned base = 0; base < 32; ++base)
			for (unsigned transfer = 0; transfer < 32; ++transfer) {
				const auto instruction = base_encoding | (base << 5) | transfer;
				auto interpreted = mte::state();
				interpreted.pc = mte::pc;
				const auto before_interpreted = interpreted;
				memory.reset_counts();
				const auto result = arm64::Dispatcher{}.execute(
					interpreted, memory, instruction);
				mte::test::require(result.reason ==
					arm64::StopReason::illegal_instruction &&
					result.instructions == 0 && result.address == mte::pc &&
					memory.tag_reads == 0 && memory.tag_writes == 0 &&
					memory.reads == 0 && memory.writes == 0);
				mte::require_state(
					interpreted, before_interpreted, mte::pc);

				auto gadget = mte::state();
				const auto before_gadget = gadget;
				mte::test::require(mapping.execute(
					mte::pc, instruction, gadget) ==
					gadgets::Event::illegal_instruction);
				mte::require_state(gadget, before_gadget, mte::pc);
			}

	for (const unsigned operation : {0U, 2U, 3U}) {
		const auto instruction = 0xd920'0000U | (operation << 22) |
			(1U << 12) | (1U << 5) | 2U;
		auto cpu = mte::state();
		cpu.pc = mte::pc;
		const auto before = cpu;
		const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
		mte::test::require(
			result.reason == arm64::StopReason::illegal_instruction);
		mte::require_state(cpu, before, mte::pc);
	}
}

void allocation_tags_follow_shared_backing_aliases()
{
	constexpr unsigned long first_generation = 0x6d7a11UL;
	constexpr unsigned long second_generation = 0x6d7a12UL;
	constexpr std::uint64_t first = 0x7800'0000ULL;
	constexpr std::uint64_t second = 0x7900'0000ULL;
	std::array<char, 64> path{};
	constexpr char pattern[] = "/tmp/uml-mte-alias-XXXXXX";
	std::memcpy(path.data(), pattern, sizeof(pattern));
	const int fd = mkstemp(path.data());
	mte::test::require(fd >= 0 && unlink(path.data()) == 0 &&
		ftruncate(fd, 2 * DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	mte::test::require(darwin_uml_static_vm_map_generation(first_generation,
		reinterpret_cast<void *>(first), fd, 0,
		2 * DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	mte::test::require(darwin_uml_static_vm_map_generation(second_generation,
		reinterpret_cast<void *>(second), fd, 0,
		2 * DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	mte::static_store_tag(first_generation, first + 0x2f, 13);
	mte::test::require(mte::static_load_tag(
		second_generation, second + 0x20) == 13);
	mte::static_store_tag(second_generation,
		second + DARWIN_UML_GUEST_PAGE_SIZE + 0x31, 6);
	mte::test::require(mte::static_load_tag(
		first_generation,
		first + DARWIN_UML_GUEST_PAGE_SIZE + 0x3f) == 6);
	mte::test::require(darwin_uml_static_vm_destroy(first_generation) == 0 &&
		darwin_uml_static_vm_destroy(second_generation) == 0 && close(fd) == 0);
}

void production_specialization()
{
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
	return;
#else
	std::set<std::uintptr_t> entries;
	for (const unsigned base : {1U, 31U})
		for (const unsigned destination : {2U, 31U}) {
			const auto built = gadgets::Builder{}.build(mte::pc,
				std::array{mte::load_tag(0, base, destination)});
			mte::test::require(built.complete &&
				built.program.instruction_count() == 1);
			entries.insert(built.program.words().front());
		}
	mte::test::require(entries.size() == 4);
#endif
}

} // namespace

int main()
{
	encoding_examples();
	interpreter_all_registers();
	gadget_all_registers();
	every_immediate_and_granule_alignment();
	fault_safety();
	privileged_and_reserved_encodings_are_early_illegal();
	allocation_tags_follow_shared_backing_aliases();
	production_specialization();
}
