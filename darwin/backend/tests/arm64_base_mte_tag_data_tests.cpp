// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_mte_memory_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <set>

namespace mte = mte_memory_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::array zero_operations{
	mte::Operation::store_zero_tag,
	mte::Operation::store_zero_two_tags,
};
constexpr std::array modes{
	mte::Mode::post_index,
	mte::Mode::signed_offset,
	mte::Mode::pre_index,
};

[[nodiscard]] constexpr std::size_t zero_size(
	mte::Operation operation) noexcept
{
	return operation == mte::Operation::store_zero_two_tags ? 32 : 16;
}

[[nodiscard]] std::array<std::byte, 16> pair_bytes(
	const arm64::CpuState &cpu, unsigned first, unsigned second)
{
	std::array<std::byte, 16> bytes{};
	const auto first_value = mte::get_register(cpu, first, false);
	const auto second_value = mte::get_register(cpu, second, false);
	std::memcpy(bytes.data(), &first_value, sizeof(first_value));
	std::memcpy(bytes.data() + 8, &second_value, sizeof(second_value));
	return bytes;
}

void encoding_examples()
{
	mte::test::require(mte::tag_memory(mte::Operation::store_zero_tag,
		mte::Mode::signed_offset, 0, 15, 14) == 0xd960'09eeU);
	mte::test::require(mte::tag_memory(mte::Operation::store_zero_tag,
		mte::Mode::post_index, 1, 31, 31) == 0xd960'17ffU);
	mte::test::require(mte::tag_memory(mte::Operation::store_zero_two_tags,
		mte::Mode::pre_index, 0x1fe, 13, 12) == 0xd9ff'edacU);
	mte::test::require(mte::store_pair_tag(
		mte::Mode::signed_offset, 0, 22, 20, 21) == 0x6900'56d4U);
	mte::test::require(mte::store_pair_tag(
		mte::Mode::post_index, 1, 31, 23, 24) == 0x6880'e3f7U);
	mte::test::require(mte::store_pair_tag(
		mte::Mode::pre_index, 0x7f, 27, 25, 26) == 0x69bf'eb79U);
}

void check_zero_interpreter(mte::Operation operation, mte::Mode mode,
	unsigned base, unsigned transfer, unsigned imm9)
{
	mte::Memory memory;
	const auto target = mte::Memory::base + 2 * 4096 + 0x300;
	auto cpu = mte::state();
	mte::set_register(cpu, base,
		(mode == mte::Mode::post_index ? target : target - mte::offset9(imm9)) |
		(static_cast<std::uint64_t>((base * 3U + 2U) & 15U) << 56), true);
	if (transfer != base)
		mte::set_register(cpu, transfer,
			(static_cast<std::uint64_t>((transfer * 9U + 5U) & 15U) << 56) |
			0x0000'1234'5678'9abcULL, true);
	cpu.pc = mte::pc;
	auto expected = cpu;
	const auto tag = static_cast<std::uint8_t>(
		mte::get_register(cpu, transfer, true) >> 56) & 15U;
	if (mode == mte::Mode::post_index || mode == mte::Mode::pre_index)
		mte::set_register(expected, base,
			mte::get_register(cpu, base, true) + mte::offset9(imm9), true);
	memory.set_tag(target, 1);
	memory.set_tag(target + 16, 2);
	std::fill_n(memory.at(target), zero_size(operation), std::byte{0xa5});
	const auto instruction = mte::tag_memory(
		operation, mode, imm9, base, transfer);
	const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
	mte::require_continued(result, instruction);
	mte::test::require(std::all_of(memory.at(target),
		memory.at(target) + zero_size(operation),
		[](std::byte byte) { return byte == std::byte{}; }) &&
		memory.writes == 1 && memory.last_data_address == target &&
		memory.last_data_size == zero_size(operation) &&
		memory.tag_writes == (zero_size(operation) / 16) &&
		memory.tag(target) == tag &&
		(zero_size(operation) == 16 || memory.tag(target + 16) == tag));
	mte::require_state(cpu, expected, mte::pc + 4);
}

void zero_store_interpreter_registers_and_immediates()
{
	for (const auto operation : zero_operations)
		for (const auto mode : modes) {
			for (unsigned base = 0; base < 32; ++base)
				for (unsigned transfer = 0; transfer < 32; ++transfer)
					check_zero_interpreter(operation, mode, base, transfer,
						(base * 29U + transfer * 13U) & 0x1ffU);
			for (unsigned imm9 = 0; imm9 < 512; ++imm9)
				check_zero_interpreter(
					operation, mode, 1, 2, imm9);
		}
}

void zero_store_gadget_registers()
{
	constexpr std::uint64_t guest = 0x7c00'0000ULL;
	constexpr unsigned long generation = 0x6d7c01UL;
	mte::test::StaticMapping mapping{
		guest, generation, "/tmp/uml-mte-zero-store-XXXXXX"};
	const auto target = guest + 2 * 4096 + 0x300;
	for (const auto operation : zero_operations)
		for (const auto mode : modes)
			for (unsigned base = 0; base < 32; ++base)
				for (unsigned transfer = 0; transfer < 32; ++transfer) {
					const unsigned imm9 =
						(base * 29U + transfer * 13U) & 0x1ffU;
					auto cpu = mte::state();
					mte::set_register(cpu, base,
						(mode == mte::Mode::post_index ? target :
						 target - mte::offset9(imm9)) |
						(static_cast<std::uint64_t>(
							(base * 3U + 2U) & 15U) << 56), true);
					if (transfer != base)
						mte::set_register(cpu, transfer,
							(static_cast<std::uint64_t>(
								(transfer * 9U + 5U) & 15U) << 56), true);
					auto expected = cpu;
					const auto tag = static_cast<std::uint8_t>(
						mte::get_register(cpu, transfer, true) >> 56) & 15U;
					if (mode == mte::Mode::post_index ||
					    mode == mte::Mode::pre_index)
						mte::set_register(expected, base,
							mte::get_register(cpu, base, true) +
							mte::offset9(imm9), true);
					std::fill_n(mapping.at(target), zero_size(operation),
						std::byte{0xa5});
					mte::static_store_tag(generation, target, 1);
					mte::static_store_tag(generation, target + 16, 2);
					const auto event = mapping.execute(mte::pc,
						mte::tag_memory(operation, mode, imm9, base, transfer),
						cpu);
					mte::test::require(event == gadgets::Event::system_call &&
						std::all_of(mapping.at(target),
							mapping.at(target) + zero_size(operation),
							[](std::byte byte) { return byte == std::byte{}; }) &&
						mte::static_load_tag(generation, target) == tag &&
						(zero_size(operation) == 16 ||
						 mte::static_load_tag(generation, target + 16) == tag));
					mte::require_state(cpu, expected, mte::pc + 8);
				}
}

void check_stgp_interpreter(mte::Mode mode, unsigned base, unsigned first,
	unsigned second, unsigned imm7)
{
	mte::Memory memory;
	const auto target = mte::Memory::base + 2 * 4096 + 0x300;
	auto cpu = mte::state();
	if (first != 31)
		cpu.registers[first] = 0x1122'3344'5566'7700ULL + first;
	if (second != 31)
		cpu.registers[second] = 0x8899'aabb'ccdd'ee00ULL + second;
	mte::set_register(cpu, base,
		(mode == mte::Mode::post_index ? target : target - mte::offset7(imm7)) |
		(static_cast<std::uint64_t>((base * 7U + 4U) & 15U) << 56), true);
	cpu.pc = mte::pc;
	auto expected = cpu;
	const auto bytes = pair_bytes(cpu, first, second);
	const auto tag = static_cast<std::uint8_t>(
		(mode == mte::Mode::post_index ? mte::get_register(cpu, base, true) :
		 mte::get_register(cpu, base, true) + mte::offset7(imm7)) >> 56) & 15U;
	if (mode == mte::Mode::post_index || mode == mte::Mode::pre_index)
		mte::set_register(expected, base,
			mte::get_register(cpu, base, true) + mte::offset7(imm7), true);
	std::fill_n(memory.at(target), 16, std::byte{0xa5});
	memory.set_tag(target, 1);
	const auto instruction = mte::store_pair_tag(
		mode, imm7, base, first, second);
	const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
	mte::require_continued(result, instruction);
	mte::test::require(std::equal(bytes.begin(), bytes.end(), memory.at(target)) &&
		memory.writes == 1 && memory.last_data_address == target &&
		memory.last_data_size == 16 && memory.tag_writes == 1 &&
		memory.tag(target) == tag);
	mte::require_state(cpu, expected, mte::pc + 4);
}

void stgp_interpreter_registers_and_immediates()
{
	for (const auto mode : modes) {
		for (unsigned base = 0; base < 32; ++base)
			for (const unsigned first : {0U, 7U, 31U})
				for (const unsigned second : {1U, 9U, 31U})
					check_stgp_interpreter(mode, base, first, second,
						(base * 11U + first * 3U + second) & 0x7fU);
		for (unsigned first = 0; first < 32; ++first)
			for (unsigned second = 0; second < 32; ++second)
				check_stgp_interpreter(mode, 5, first, second,
					(first * 17U + second * 5U) & 0x7fU);
		for (unsigned imm7 = 0; imm7 < 128; ++imm7)
			check_stgp_interpreter(mode, 6, 7, 8, imm7);
	}
}

void stgp_gadget_registers()
{
	constexpr std::uint64_t guest = 0x7d00'0000ULL;
	constexpr unsigned long generation = 0x6d7c02UL;
	mte::test::StaticMapping mapping{
		guest, generation, "/tmp/uml-mte-stgp-XXXXXX"};
	const auto target = guest + 2 * 4096 + 0x300;
	const auto check = [&](mte::Mode mode, unsigned base, unsigned first,
		unsigned second, unsigned imm7) {
		auto cpu = mte::state();
		if (first != 31)
			cpu.registers[first] = 0x1122'3344'5566'7700ULL + first;
		if (second != 31)
			cpu.registers[second] = 0x8899'aabb'ccdd'ee00ULL + second;
		mte::set_register(cpu, base,
			(mode == mte::Mode::post_index ? target :
			 target - mte::offset7(imm7)) |
			(static_cast<std::uint64_t>((base * 7U + 4U) & 15U) << 56), true);
		auto expected = cpu;
		const auto bytes = pair_bytes(cpu, first, second);
		const auto tag = static_cast<std::uint8_t>(
			(mode == mte::Mode::post_index ?
			 mte::get_register(cpu, base, true) :
			 mte::get_register(cpu, base, true) + mte::offset7(imm7)) >> 56) &
			15U;
		if (mode == mte::Mode::post_index || mode == mte::Mode::pre_index)
			mte::set_register(expected, base,
				mte::get_register(cpu, base, true) + mte::offset7(imm7), true);
		std::fill_n(mapping.at(target), 16, std::byte{0xa5});
		mte::static_store_tag(generation, target, 1);
		const auto event = mapping.execute(mte::pc,
			mte::store_pair_tag(mode, imm7, base, first, second), cpu);
		mte::test::require(event == gadgets::Event::system_call &&
			std::equal(bytes.begin(), bytes.end(), mapping.at(target)) &&
			mte::static_load_tag(generation, target) == tag);
		mte::require_state(cpu, expected, mte::pc + 8);
	};
	for (const auto mode : modes) {
		for (unsigned base = 0; base < 32; ++base)
			for (const unsigned first : {0U, 7U, 31U})
				for (const unsigned second : {1U, 9U, 31U})
					check(mode, base, first, second,
						(base * 11U + first * 3U + second) & 0x7fU);
		for (unsigned first = 0; first < 32; ++first)
			for (unsigned second = 0; second < 32; ++second)
				check(mode, 5, first, second,
					(first * 17U + second * 5U) & 0x7fU);
	}
}

void interpreter_fault_ordering()
{
	const auto address = mte::Memory::base + 2 * 4096 + 0x300;
	for (const auto operation : zero_operations) {
		mte::Memory memory;
		std::fill_n(memory.at(address), zero_size(operation), std::byte{0xa5});
		memory.fault_write = address;
		auto cpu = mte::state();
		cpu.registers[1] = address;
		cpu.registers[2] = 0x0d00'0000'0000'0000ULL;
		cpu.pc = mte::pc;
		const auto before = cpu;
		const auto instruction = mte::tag_memory(
			operation, mte::Mode::post_index, 1, 1, 2);
		const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
		mte::test::require(result.reason == arm64::StopReason::memory_fault &&
			result.address == address && result.write && memory.writes == 1 &&
			memory.tag_writes == 0 &&
			std::all_of(memory.at(address),
				memory.at(address) + zero_size(operation),
				[](std::byte byte) { return byte == std::byte{0xa5}; }));
		mte::require_state(cpu, before, mte::pc);
	}

	mte::Memory zero_tag_fault;
	std::fill_n(zero_tag_fault.at(address), 16, std::byte{0xa5});
	zero_tag_fault.fault_tag_write = address;
	auto zero_cpu = mte::state();
	zero_cpu.registers[1] = address;
	zero_cpu.registers[2] = 0x0e00'0000'0000'0000ULL;
	zero_cpu.pc = mte::pc;
	const auto zero_before = zero_cpu;
	const auto zero_result = arm64::Dispatcher{}.execute(zero_cpu,
		zero_tag_fault, mte::tag_memory(mte::Operation::store_zero_tag,
			mte::Mode::post_index, 1, 1, 2));
	mte::test::require(zero_result.reason == arm64::StopReason::memory_fault &&
		std::all_of(zero_tag_fault.at(address),
			zero_tag_fault.at(address) + 16,
			[](std::byte byte) { return byte == std::byte{}; }) &&
		zero_tag_fault.writes == 1 && zero_tag_fault.tag_writes == 1);
	mte::require_state(zero_cpu, zero_before, mte::pc);

	mte::Memory pair_tag_fault;
	std::fill_n(pair_tag_fault.at(address), 16, std::byte{0xa5});
	pair_tag_fault.fault_tag_write = address;
	auto pair_cpu = mte::state();
	pair_cpu.registers[1] = address | 0x0b00'0000'0000'0000ULL;
	pair_cpu.pc = mte::pc;
	const auto pair_before = pair_cpu;
	const auto expected_bytes = pair_bytes(pair_cpu, 2, 3);
	const auto pair_result = arm64::Dispatcher{}.execute(pair_cpu,
		pair_tag_fault, mte::store_pair_tag(
			mte::Mode::post_index, 1, 1, 2, 3));
	mte::test::require(pair_result.reason == arm64::StopReason::memory_fault &&
		std::equal(expected_bytes.begin(), expected_bytes.end(),
			pair_tag_fault.at(address)) && pair_tag_fault.writes == 1 &&
		pair_tag_fault.tag_writes == 1);
	mte::require_state(pair_cpu, pair_before, mte::pc);
}

void alignment_and_gadget_cross_page_fault_safety()
{
	const auto address = mte::Memory::base + 0x301;
	for (const auto instruction : {
	     mte::tag_memory(mte::Operation::store_zero_tag,
		mte::Mode::signed_offset, 0, 1, 2),
	     mte::tag_memory(mte::Operation::store_zero_two_tags,
		mte::Mode::signed_offset, 0, 1, 2),
	     mte::store_pair_tag(mte::Mode::signed_offset, 0, 1, 2, 3)}) {
		mte::Memory memory;
		auto cpu = mte::state();
		cpu.registers[1] = address;
		cpu.pc = mte::pc;
		const auto before = cpu;
		const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
		mte::test::require(result.reason == arm64::StopReason::memory_fault &&
			result.address == address && result.write &&
			memory.writes == 0 && memory.tag_writes == 0);
		mte::require_state(cpu, before, mte::pc);
	}

	constexpr std::uint64_t guest = 0x7e00'0000ULL;
	mte::test::StaticMapping mapping{
		guest, 0x6d7c03UL, "/tmp/uml-mte-cross-fault-XXXXXX"};
	const auto boundary = guest + mte::test::StaticMapping::length - 16;
	std::fill_n(mapping.at(boundary), 16, std::byte{0xa5});
	auto cpu = mte::state();
	cpu.registers[1] = boundary;
	cpu.registers[2] = 0x0c00'0000'0000'0000ULL;
	const auto before = cpu;
	mte::test::require(mapping.execute(mte::pc,
		mte::tag_memory(mte::Operation::store_zero_two_tags,
			mte::Mode::signed_offset, 0, 1, 2), cpu) ==
		gadgets::Event::memory_fault);
	mte::test::require(std::all_of(mapping.at(boundary),
		mapping.at(boundary) + 16,
		[](std::byte byte) { return byte == std::byte{0xa5}; }));
	mte::require_state(cpu, before, mte::pc);
}

void production_specialization()
{
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
	return;
#else
	std::set<std::uintptr_t> zero_entries;
	for (const auto operation : zero_operations)
		for (const auto mode : modes)
			for (const unsigned base : {1U, 31U}) {
				const auto built = gadgets::Builder{}.build(mte::pc,
					std::array{mte::tag_memory(
						operation, mode, 0, base, 2)});
				mte::test::require(built.complete);
				zero_entries.insert(built.program.words().front());
			}
	mte::test::require(zero_entries.size() == 12);

	std::set<std::uintptr_t> pair_entries;
	for (const auto mode : modes)
		for (const unsigned base : {1U, 31U})
			for (const unsigned first : {2U, 31U})
				for (const unsigned second : {3U, 31U}) {
					const auto built = gadgets::Builder{}.build(mte::pc,
						std::array{mte::store_pair_tag(
							mode, 0, base, first, second)});
					mte::test::require(built.complete);
					pair_entries.insert(built.program.words().front());
				}
	mte::test::require(pair_entries.size() == 24);
#endif
}

} // namespace

int main()
{
	encoding_examples();
	zero_store_interpreter_registers_and_immediates();
	zero_store_gadget_registers();
	stgp_interpreter_registers_and_immediates();
	stgp_gadget_registers();
	interpreter_fault_ordering();
	alignment_and_gadget_cross_page_fault_safety();
	production_specialization();
}
