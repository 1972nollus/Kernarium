// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_test_support.hpp"

#include <array>
#include <cstring>

namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x70'0000;
constexpr std::uint64_t base = 0x20'0000;
constexpr std::uint64_t store_value = 0xf1e2'd3c4'b5a6'9788ULL;

void check_transfer(const test::IntegerVariant &variant,
	std::uint32_t instruction, std::uint64_t expected_address,
	bool writeback, std::uint64_t adjusted)
{
	test::ProbeMemory memory;
	arm64::CpuState cpu{};
	test::fill_registers(cpu);
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[3] = store_value;
	const auto old_sp = cpu.sp;
	const auto old_nzcv = cpu.nzcv;
	const auto old_fpcr = cpu.fpcr;
	const auto old_fpsr = cpu.fpsr;
	const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
	test::require(result.reason == arm64::StopReason::continued);
	test::require(cpu.pc == pc + 4);
	test::require(memory.last_address == expected_address);
	test::require(memory.last_size == variant.size);
	test::require(cpu.registers[2] == (writeback ? adjusted : base));
	test::require(cpu.sp == old_sp);
	test::require(cpu.nzcv == old_nzcv);
	test::require(cpu.fpcr == old_fpcr);
	test::require(cpu.fpsr == old_fpsr);
	if (variant.load) {
		test::require(memory.reads == 1 && memory.writes == 0);
		test::require(cpu.registers[3] == test::expected_integer_load(
			variant, test::payload_value(memory, variant.size)));
	} else {
		test::require(memory.reads == 0 && memory.writes == 1);
		test::require(std::memcmp(memory.written.data(), &store_value,
			variant.size) == 0);
		test::require(cpu.registers[3] == store_value);
	}
}

void exhaustive_interpreter_offsets()
{
	for (const auto &variant : test::integer_variants) {
		for (unsigned imm12 = 0; imm12 < 4096; ++imm12) {
			const auto address = base + imm12 * variant.size;
			check_transfer(variant, test::unsigned_immediate(
				variant.size_code, variant.operation, imm12, 2, 3),
				address, false, base);
		}

		for (unsigned mode = 0; mode < 4; ++mode)
			for (unsigned imm9 = 0; imm9 < 512; ++imm9) {
				const auto displacement = test::signed_imm9(imm9);
				const auto adjusted = base + displacement;
				const auto address = mode == 1 ? base : adjusted;
				const bool writeback = mode == 1 || mode == 3;
				check_transfer(variant, test::immediate_other(
					variant.size_code, variant.operation, imm9,
					mode, 2, 3), address, writeback, adjusted);
			}
	}
}

void signed_load_widths_and_special_registers()
{
	for (const auto &variant : test::integer_variants) {
		if (variant.load && variant.operation >= 2) {
			test::ProbeMemory memory;
			memory.payload.fill(std::byte{});
			memory.payload[0] = std::byte{0x7f};
			if (variant.size > 1)
				memory.payload[variant.size - 1] = std::byte{0x7f};
			arm64::CpuState cpu{};
			cpu.pc = pc;
			cpu.registers[4] = base;
			cpu.registers[5] = ~std::uint64_t{0};
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				test::unsigned_immediate(variant.size_code,
					variant.operation, 0, 4, 5)).reason ==
				arm64::StopReason::continued);
			test::require(cpu.registers[5] == test::expected_integer_load(
				variant, test::payload_value(memory, variant.size)));
		}

		test::ProbeMemory memory;
		arm64::CpuState cpu{};
		test::fill_registers(cpu);
		cpu.pc = pc;
		cpu.sp = base;
		const auto registers = cpu.registers;
		const auto instruction = test::unsigned_immediate(
			variant.size_code, variant.operation, 0, 31, 31);
		test::require(arm64::Dispatcher{}.execute(cpu, memory, instruction).reason ==
			arm64::StopReason::continued);
		test::require(cpu.sp == base);
		test::require(cpu.registers == registers);
		if (variant.load) {
			test::require(memory.reads == 1 && memory.writes == 0);
		} else {
			std::array<std::byte, 8> zero{};
			test::require(memory.reads == 0 && memory.writes == 1);
			test::require(std::memcmp(memory.written.data(), zero.data(),
				variant.size) == 0);
		}
	}
}

void overlap_policy_and_fault_atomicity()
{
	for (const auto &variant : test::integer_variants)
		for (const unsigned mode : {1U, 3U}) {
			const unsigned imm9 = 17;
			const auto adjusted = base + test::signed_imm9(imm9);
			test::ProbeMemory memory;
			arm64::CpuState cpu{};
			cpu.pc = pc;
			cpu.registers[5] = base;
			const auto instruction = test::immediate_other(
				variant.size_code, variant.operation, imm9, mode, 5, 5);
			test::require(arm64::Dispatcher{}.execute(
				cpu, memory, instruction).reason ==
				arm64::StopReason::continued);
			if (variant.load) {
				/* 2026-06 ConstrainUnpredictable selects WBSUPPRESS. */
				test::require(cpu.registers[5] == test::expected_integer_load(
					variant, test::payload_value(memory, variant.size)));
			} else {
				/* Store overlap uses Constraint_NONE: old base, then writeback. */
				test::require(std::memcmp(memory.written.data(), &base,
					variant.size) == 0);
				test::require(cpu.registers[5] == adjusted);
			}

			memory = {};
			memory.fault_read = true;
			memory.fault_write = true;
			cpu = {};
			cpu.pc = pc;
			cpu.registers[2] = base;
			cpu.registers[3] = store_value;
			const auto before = cpu;
			const auto fault_instruction = test::immediate_other(
				variant.size_code, variant.operation, imm9, mode, 2, 3);
			const auto result = arm64::Dispatcher{}.execute(
				cpu, memory, fault_instruction);
			test::require(result.reason == arm64::StopReason::memory_fault);
			test::require(result.address == (mode == 1 ? base : adjusted));
			test::require(result.write == !variant.load);
			test::require(cpu.registers == before.registers);
			test::require(cpu.sp == before.sp);
			test::require(cpu.pc == before.pc);
		}
}

void reserved_encodings_are_illegal()
{
	test::ProbeMemory memory;
	for (const auto [size_code, operation] :
	     {std::array{2U, 3U}, std::array{3U, 3U}})
		for (unsigned imm12 = 0; imm12 < 4096; ++imm12) {
			arm64::CpuState cpu{};
			cpu.pc = pc;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				test::unsigned_immediate(size_code, operation,
					imm12, 2, 3)).reason ==
				arm64::StopReason::illegal_instruction);
			test::require(cpu.pc == pc);
		}

	for (const auto [size_code, operation] :
	     {std::array{2U, 3U}, std::array{3U, 3U}})
		for (unsigned mode = 0; mode < 4; ++mode)
			for (unsigned imm9 = 0; imm9 < 512; ++imm9) {
				arm64::CpuState cpu{};
				cpu.pc = pc;
				test::require(arm64::Dispatcher{}.execute(cpu, memory,
					test::immediate_other(size_code, operation,
						imm9, mode, 2, 3)).reason ==
					arm64::StopReason::illegal_instruction);
			}
	for (const unsigned mode : {1U, 2U, 3U})
		for (unsigned imm9 = 0; imm9 < 512; ++imm9) {
			arm64::CpuState cpu{};
			cpu.pc = pc;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				test::immediate_other(3, 2, imm9, mode, 2, 3)).reason ==
				arm64::StopReason::illegal_instruction);
		}
	test::require(memory.reads == 0 && memory.writes == 0);
}

void gadget_immediate_matrix()
{
	test::StaticMapping mapping{0x30'0000, 43,
		"/private/tmp/uml-base-ls-imm.XXXXXX"};
	const auto map_base = mapping.guest() + 2 * test::StaticMapping::page_size;
	for (const auto &variant : test::integer_variants) {
		for (const unsigned imm12 : {0U, 1U, 4095U}) {
			const auto address = map_base + imm12 * variant.size;
			arm64::CpuState cpu{};
			cpu.registers[2] = map_base;
			cpu.registers[3] = store_value;
			test::ProbeMemory payload;
			if (variant.load)
				std::memcpy(mapping.at(address), payload.payload.data(),
					variant.size);
			else
				std::memset(mapping.at(address), 0, variant.size);
			const auto instruction = test::unsigned_immediate(
				variant.size_code, variant.operation, imm12, 2, 3);
			test::require(mapping.execute(pc, instruction, cpu) ==
				gadgets::Event::system_call);
			test::require(cpu.pc == pc + 8);
			if (variant.load) {
				test::require(cpu.registers[3] == test::expected_integer_load(
					variant, test::payload_value(payload, variant.size)));
			} else {
				test::require(std::memcmp(mapping.at(address), &store_value,
					variant.size) == 0);
			}
		}

		for (unsigned mode = 0; mode < 4; ++mode)
			for (const unsigned imm9 : {0U, 1U, 255U, 256U, 511U}) {
				const auto adjusted = map_base + test::signed_imm9(imm9);
				const auto address = mode == 1 ? map_base : adjusted;
				arm64::CpuState cpu{};
				cpu.registers[2] = map_base;
				cpu.registers[3] = store_value;
				test::ProbeMemory payload;
				std::memcpy(mapping.at(address), payload.payload.data(),
					variant.size);
				const auto instruction = test::immediate_other(
					variant.size_code, variant.operation, imm9,
					mode, 2, 3);
				test::require(mapping.execute(pc, instruction, cpu) ==
					gadgets::Event::system_call);
				if (variant.load)
					test::require(cpu.registers[3] ==
						test::expected_integer_load(variant,
							test::payload_value(payload, variant.size)));
				else
					test::require(std::memcmp(mapping.at(address), &store_value,
						variant.size) == 0);
				const bool writeback = mode == 1 || mode == 3;
				test::require(cpu.registers[2] ==
					(writeback ? adjusted : map_base));
			}
	}
}

void gadget_overlap_cross_page_and_faults()
{
	test::StaticMapping mapping{0x50'0000, 47,
		"/private/tmp/uml-base-ls-edge.XXXXXX"};
	const auto map_base = mapping.guest() + 3 * test::StaticMapping::page_size;
	for (const auto &variant : test::integer_variants) {
		const auto instruction = test::immediate_other(
			variant.size_code, variant.operation, 9, 3, 5, 5);
		arm64::CpuState cpu{};
		cpu.registers[5] = map_base;
		test::ProbeMemory payload;
		std::memcpy(mapping.at(map_base + 9), payload.payload.data(),
			variant.size);
		test::require(mapping.execute(pc, instruction, cpu) ==
			gadgets::Event::system_call);
		if (variant.load)
			test::require(cpu.registers[5] == test::expected_integer_load(
				variant, test::payload_value(payload, variant.size)));
		else {
			test::require(std::memcmp(mapping.at(map_base + 9), &map_base,
				variant.size) == 0);
			test::require(cpu.registers[5] == map_base + 9);
		}

		if (variant.size > 1) {
			const auto cross = mapping.guest() +
				5 * test::StaticMapping::page_size - variant.size / 2;
			cpu = {};
			cpu.registers[2] = cross;
			cpu.registers[3] = store_value;
			std::memcpy(mapping.at(cross), payload.payload.data(), variant.size);
			test::require(mapping.execute(pc, test::immediate_other(
				variant.size_code, variant.operation, 0, 0, 2, 3), cpu) ==
				gadgets::Event::system_call);
			if (variant.load)
				test::require(cpu.registers[3] == test::expected_integer_load(
					variant, test::payload_value(payload, variant.size)));
			else
				test::require(std::memcmp(mapping.at(cross), &store_value,
					variant.size) == 0);
		}

		const auto fault_address = mapping.guest() + test::StaticMapping::length +
			0x100;
		cpu = {};
		cpu.registers[2] = fault_address;
		cpu.registers[3] = store_value;
		const auto before = cpu;
		test::require(mapping.execute(pc, test::immediate_other(
			variant.size_code, variant.operation, 7, 1, 2, 3), cpu) ==
			gadgets::Event::memory_fault);
		test::require(cpu.pc == pc);
		test::require(cpu.registers == before.registers);
	}

	for (const auto [size_code, operation] :
	     {std::array{2U, 3U}, std::array{3U, 3U}}) {
		arm64::CpuState cpu{};
		test::require(mapping.execute(pc, test::unsigned_immediate(
			size_code, operation, 0, 2, 3), cpu) ==
			gadgets::Event::illegal_instruction);
		test::require(cpu.pc == pc);
	}
}

} // namespace

int main()
{
	exhaustive_interpreter_offsets();
	signed_load_widths_and_special_registers();
	overlap_policy_and_fault_atomicity();
	reserved_encodings_are_illegal();
	gadget_immediate_matrix();
	gadget_overlap_cross_page_and_faults();
}
