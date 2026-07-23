// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_test_support.hpp"

#include <array>
#include <cstring>

namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x90'0000;
constexpr std::uint64_t base = 0x40'0000;
constexpr std::uint64_t store_value = 0x8877'6655'4433'2211ULL;

void check_register_transfer(const test::IntegerVariant &variant,
	unsigned option, bool scaled, unsigned rm, std::uint64_t raw_offset)
{
	test::ProbeMemory memory;
	arm64::CpuState cpu{};
	test::fill_registers(cpu);
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[3] = store_value;
	if (rm < 31)
		cpu.registers[rm] = raw_offset;
	const auto actual_base = rm == 2 ? raw_offset : base;
	const auto actual_store = rm == 3 ? raw_offset : store_value;
	const auto offset = rm == 31 ? 0 : test::extend_offset(raw_offset,
		option, scaled ? variant.size_code : 0);
	const auto expected_address = actual_base + offset;
	const auto instruction = test::register_offset(variant.size_code,
		variant.operation, rm, option, scaled, 2, 2, 3);
	const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
	test::require(result.reason == arm64::StopReason::continued);
	test::require(cpu.pc == pc + 4);
	test::require(memory.last_address == expected_address);
	test::require(memory.last_size == variant.size);
	test::require(cpu.registers[2] == actual_base);
	if (variant.load) {
		test::require(memory.reads == 1 && memory.writes == 0);
		test::require(cpu.registers[3] == test::expected_integer_load(
			variant, test::payload_value(memory, variant.size)));
	} else {
		test::require(memory.reads == 0 && memory.writes == 1);
		test::require(std::memcmp(memory.written.data(), &actual_store,
			variant.size) == 0);
	}
}

void exhaustive_extensions_and_fields()
{
	constexpr std::array raw_values{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0x7fff'ffffU},
		std::uint64_t{0x8000'0000U}, std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL}};
	for (const auto &variant : test::integer_variants)
		for (const unsigned option : {2U, 3U, 6U, 7U})
			for (const bool scaled : {false, true})
				for (const auto raw : raw_values)
					check_register_transfer(
						variant, option, scaled, 4, raw);

	for (const auto &variant : test::integer_variants)
		for (const unsigned option : {2U, 3U, 6U, 7U})
			for (const bool scaled : {false, true})
				for (unsigned rm = 0; rm < 32; ++rm)
					check_register_transfer(variant, option, scaled,
						rm, 0x8000'0000'0000'0000ULL | rm);
}

void aliases_and_special_registers()
{
	for (const auto &variant : test::integer_variants) {
		/* XZR is a zero index and Rt==31 discards loads/stores zero. */
		test::ProbeMemory memory;
		arm64::CpuState cpu{};
		test::fill_registers(cpu);
		cpu.pc = pc;
		cpu.sp = base;
		const auto registers = cpu.registers;
		const auto instruction = test::register_offset(variant.size_code,
			variant.operation, 31, 3, true, 2, 31, 31, false);
		test::require(arm64::Dispatcher{}.execute(cpu, memory, instruction).reason ==
			arm64::StopReason::continued);
		test::require(memory.last_address == base);
		test::require(cpu.sp == base);
		test::require(cpu.registers == registers);
		if (!variant.load) {
			std::array<std::byte, 8> zero{};
			test::require(std::memcmp(memory.written.data(), zero.data(),
				variant.size) == 0);
		}

		/* Rm==Rt: the offset is consumed before a load replaces Rt. */
		memory = {};
		cpu = {};
		cpu.pc = pc;
		cpu.registers[2] = base;
		cpu.registers[4] = 7;
		const auto rm_rt = test::register_offset(variant.size_code,
			variant.operation, 4, 3, false, 2, 2, 4, false);
		test::require(arm64::Dispatcher{}.execute(cpu, memory, rm_rt).reason ==
			arm64::StopReason::continued);
		test::require(memory.last_address == base + 7);
		if (variant.load)
			test::require(cpu.registers[4] == test::expected_integer_load(
				variant, test::payload_value(memory, variant.size)));
		else
			test::require(std::memcmp(memory.written.data(), &cpu.registers[4],
				variant.size) == 0);

		/* Rn==Rt has no writeback and stores the original base value. */
		memory = {};
		cpu = {};
		cpu.pc = pc;
		cpu.registers[5] = base;
		const auto rn_rt = test::register_offset(variant.size_code,
			variant.operation, 31, 3, false, 2, 5, 5, false);
		test::require(arm64::Dispatcher{}.execute(cpu, memory, rn_rt).reason ==
			arm64::StopReason::continued);
		if (variant.load)
			test::require(cpu.registers[5] == test::expected_integer_load(
				variant, test::payload_value(memory, variant.size)));
		else
			test::require(std::memcmp(memory.written.data(), &base,
				variant.size) == 0);
	}
}

void reserved_encodings_are_illegal()
{
	test::ProbeMemory memory;
	for (const auto &variant : test::integer_variants) {
		/* mode 00 shares this major encoding with FEAT_LSE atomics and is
		 * exhaustively classified by the split atomic suites. Only modes 01
		 * and 11 remain reserved register-offset encodings. */
		for (const unsigned mode : {1U, 3U})
			for (unsigned rm = 0; rm < 32; ++rm)
				for (unsigned option = 0; option < 8; ++option)
					for (const bool scaled : {false, true}) {
						arm64::CpuState cpu{};
						cpu.pc = pc;
						const auto instruction = test::register_offset(
							variant.size_code, variant.operation, rm, option,
							scaled, mode, 2, 3);
						/* This apparent mode-01/mode-11 overlap is the allocated
						 * LDRAA/LDRAB immediate space, exhaustively covered by the
						 * pointer-authenticated load suite. */
						if ((instruction & 0xff20'0400U) == 0xf820'0400U)
							continue;
						const auto result = arm64::Dispatcher{}.execute(
							cpu, memory, instruction);
						if (result.reason != arm64::StopReason::illegal_instruction)
							std::fprintf(stderr,
								"reserved register-offset instruction %#x continued\n",
								instruction);
						test::require(result.reason ==
							arm64::StopReason::illegal_instruction);
						test::require(cpu.pc == pc);
					}
		for (const unsigned option : {0U, 1U, 4U, 5U})
			for (unsigned rm = 0; rm < 32; ++rm)
				for (const bool scaled : {false, true}) {
					arm64::CpuState cpu{};
					cpu.pc = pc;
					test::require(arm64::Dispatcher{}.execute(cpu, memory,
						test::register_offset(variant.size_code,
							variant.operation, rm, option, scaled,
							2, 2, 3)).reason ==
						arm64::StopReason::illegal_instruction);
				}
	}

	for (const auto [size_code, operation] :
	     {std::array{2U, 3U}, std::array{3U, 3U}})
		for (const unsigned option : {2U, 3U, 6U, 7U}) {
			arm64::CpuState cpu{};
			cpu.pc = pc;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				test::register_offset(size_code, operation, 4, option,
					true, 2, 2, 3)).reason ==
				arm64::StopReason::illegal_instruction);
		}
	test::require(memory.reads == 0 && memory.writes == 0);
}

void fault_atomicity()
{
	for (const auto &variant : test::integer_variants) {
		test::ProbeMemory memory;
		memory.fault_read = true;
		memory.fault_write = true;
		arm64::CpuState cpu{};
		test::fill_registers(cpu);
		cpu.pc = pc;
		cpu.registers[2] = base;
		cpu.registers[3] = store_value;
		cpu.registers[4] = 13;
		const auto before = cpu;
		const auto instruction = test::register_offset(variant.size_code,
			variant.operation, 4, 3, true, 2, 2, 3, false);
		const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
		test::require(result.reason == arm64::StopReason::memory_fault);
		test::require(result.address == base + (13 << variant.size_code));
		test::require(result.write == !variant.load);
		test::require(cpu.registers == before.registers);
		test::require(cpu.pc == before.pc);
	}
}

void gadget_register_matrix()
{
	test::StaticMapping mapping{0x60'0000, 53,
		"/private/tmp/uml-base-ls-reg.XXXXXX"};
	const auto map_base = mapping.guest() +
		10 * test::StaticMapping::page_size;
	for (const auto &variant : test::integer_variants)
		for (const unsigned option : {2U, 3U, 6U, 7U})
			for (const bool scaled : {false, true}) {
				const std::uint64_t raw = option >= 6 ?
					std::uint64_t{0xffff'ffff'ffff'ffffULL} : 3U;
				const auto offset = test::extend_offset(raw, option,
					scaled ? variant.size_code : 0);
				const auto address = map_base + offset;
				arm64::CpuState cpu{};
				cpu.registers[2] = map_base;
				cpu.registers[3] = store_value;
				cpu.registers[4] = raw;
				test::ProbeMemory payload;
				std::memcpy(mapping.at(address), payload.payload.data(),
					variant.size);
				const auto instruction = test::register_offset(
					variant.size_code, variant.operation, 4, option,
					scaled, 2, 2, 3, false);
				test::require(mapping.execute(pc, instruction, cpu) ==
					gadgets::Event::system_call);
				if (variant.load)
					test::require(cpu.registers[3] ==
						test::expected_integer_load(variant,
							test::payload_value(payload, variant.size)));
				else
					test::require(std::memcmp(mapping.at(address), &store_value,
						variant.size) == 0);
			}
}

void gadget_special_cross_page_fault_and_illegal()
{
	test::StaticMapping mapping{0x80'0000, 59,
		"/private/tmp/uml-base-ls-reg-edge.XXXXXX"};
	for (const auto &variant : test::integer_variants) {
		const auto cross = mapping.guest() +
			4 * test::StaticMapping::page_size -
			(variant.size > 1 ? variant.size / 2 : 0);
		arm64::CpuState cpu{};
		cpu.sp = cross;
		cpu.registers[3] = store_value;
		test::ProbeMemory payload;
		std::memcpy(mapping.at(cross), payload.payload.data(), variant.size);
		test::require(mapping.execute(pc, test::register_offset(
			variant.size_code, variant.operation, 31, 3, true,
			2, 31, 3, false), cpu) == gadgets::Event::system_call);
		if (variant.load)
			test::require(cpu.registers[3] == test::expected_integer_load(
				variant, test::payload_value(payload, variant.size)));
		else
			test::require(std::memcmp(mapping.at(cross), &store_value,
				variant.size) == 0);

		const auto fault = mapping.guest() + test::StaticMapping::length + 64;
		cpu = {};
		cpu.registers[2] = fault;
		cpu.registers[3] = store_value;
		const auto before = cpu.registers;
		test::require(mapping.execute(pc, test::register_offset(
			variant.size_code, variant.operation, 31, 3, false,
			2, 2, 3, false), cpu) == gadgets::Event::memory_fault);
		test::require(cpu.pc == pc);
		test::require(cpu.registers == before);
	}

	for (const unsigned mode : {1U, 3U}) {
		arm64::CpuState cpu{};
		test::require(mapping.execute(pc, test::register_offset(
			2, 1, 4, 3, false, mode, 2, 3, false), cpu) ==
			gadgets::Event::illegal_instruction);
		test::require(cpu.pc == pc);
	}
	for (const unsigned option : {0U, 1U, 4U, 5U}) {
		arm64::CpuState cpu{};
		test::require(mapping.execute(pc, test::register_offset(
			2, 1, 4, option, false, 2, 2, 3, false), cpu) ==
			gadgets::Event::illegal_instruction);
	}
}

} // namespace

int main()
{
	exhaustive_extensions_and_fields();
	aliases_and_special_registers();
	reserved_encodings_are_illegal();
	fault_atomicity();
	gadget_register_matrix();
	gadget_special_cross_page_fault_and_illegal();
}
