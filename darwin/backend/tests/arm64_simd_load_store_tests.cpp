// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0xb0'0000;
constexpr std::uint64_t base = 0x60'0000;

[[nodiscard]] arm64::VectorRegister vector_pattern()
{
	arm64::VectorRegister value;
	for (unsigned index = 0; index < value.bytes.size(); ++index)
		value.bytes[index] = static_cast<std::byte>(0xf1U - index * 11U);
	return value;
}

void require_loaded_vector(const arm64::VectorRegister &actual,
	const test::ProbeMemory &memory, std::size_t size)
{
	for (unsigned index = 0; index < actual.bytes.size(); ++index)
		test::require(actual.bytes[index] ==
			(index < size ? memory.payload[index] : std::byte{}));
}

void check_immediate(const test::VectorVariant &variant,
	std::uint32_t instruction, std::uint64_t expected_address,
	bool writeback, std::uint64_t adjusted)
{
	test::ProbeMemory memory;
	arm64::CpuState cpu{};
	test::fill_registers(cpu);
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.vectors[3] = vector_pattern();
	const auto source = cpu.vectors[3];
	const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
	test::require(result.reason == arm64::StopReason::continued);
	test::require(cpu.pc == pc + 4);
	test::require(memory.last_address == expected_address);
	test::require(memory.last_size == variant.size);
	test::require(cpu.registers[2] == (writeback ? adjusted : base));
	if (variant.load) {
		test::require(memory.reads == 1 && memory.writes == 0);
		require_loaded_vector(cpu.vectors[3], memory, variant.size);
	} else {
		test::require(memory.reads == 0 && memory.writes == 1);
		test::require(std::memcmp(memory.written.data(), source.bytes.data(),
			variant.size) == 0);
		test::require(cpu.vectors[3].bytes == source.bytes);
	}
}

void exhaustive_interpreter_immediates()
{
	for (const auto &variant : test::vector_variants) {
		for (unsigned imm12 = 0; imm12 < 4096; ++imm12)
			check_immediate(variant, test::unsigned_immediate(
				variant.size_code, variant.operation, imm12, 2, 3, true),
				base + imm12 * variant.size, false, base);
		for (const unsigned mode : {0U, 1U, 3U})
			for (unsigned imm9 = 0; imm9 < 512; ++imm9) {
				const auto adjusted = base + test::signed_imm9(imm9);
				check_immediate(variant, test::immediate_other(
					variant.size_code, variant.operation, imm9,
					mode, 2, 3, true),
					mode == 1 ? base : adjusted,
					mode == 1 || mode == 3, adjusted);
			}
	}
}

void exhaustive_interpreter_register_offsets()
{
	constexpr std::array raw_values{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0x7fff'ffffU},
		std::uint64_t{0x8000'0000U}, std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		~std::uint64_t{0}};
	for (const auto &variant : test::vector_variants)
		for (const unsigned option : {2U, 3U, 6U, 7U})
			for (const bool scaled : {false, true})
				for (const auto raw : raw_values) {
					test::ProbeMemory memory;
					arm64::CpuState cpu{};
					cpu.pc = pc;
					cpu.registers[2] = base;
					cpu.registers[4] = raw;
					cpu.vectors[3] = vector_pattern();
					const auto source = cpu.vectors[3];
					const unsigned scale = variant.size == 16 ? 4 :
						variant.size_code;
					const auto address = base + test::extend_offset(
						raw, option, scaled ? scale : 0);
					const auto instruction = test::register_offset(
						variant.size_code, variant.operation, 4,
						option, scaled, 2, 2, 3, true);
					test::require(arm64::Dispatcher{}.execute(
						cpu, memory, instruction).reason ==
						arm64::StopReason::continued);
					test::require(memory.last_address == address);
					if (variant.load)
						require_loaded_vector(
							cpu.vectors[3], memory, variant.size);
					else
						test::require(std::memcmp(memory.written.data(),
							source.bytes.data(), variant.size) == 0);
				}

	for (const auto &variant : test::vector_variants)
		for (unsigned rm = 0; rm < 32; ++rm) {
			test::ProbeMemory memory;
			arm64::CpuState cpu{};
			cpu.pc = pc;
			cpu.registers[2] = base;
			if (rm < 31)
				cpu.registers[rm] = rm;
			cpu.vectors[31] = vector_pattern();
			const auto instruction = test::register_offset(
				variant.size_code, variant.operation, rm, 3, false,
				2, 2, 31, true);
			test::require(arm64::Dispatcher{}.execute(
				cpu, memory, instruction).reason ==
				arm64::StopReason::continued);
			const auto actual_base = rm == 2 ? std::uint64_t{2} : base;
			test::require(memory.last_address == actual_base +
				(rm == 31 ? 0 : rm));
		}
}

void special_registers_and_faults()
{
	for (const auto &variant : test::vector_variants) {
		test::ProbeMemory memory;
		arm64::CpuState cpu{};
		cpu.pc = pc;
		cpu.sp = base;
		cpu.vectors[31] = vector_pattern();
		const auto instruction = test::register_offset(
			variant.size_code, variant.operation, 31, 3, true,
			2, 31, 31, true);
		test::require(arm64::Dispatcher{}.execute(cpu, memory, instruction).reason ==
			arm64::StopReason::continued);
		test::require(memory.last_address == base);
		test::require(cpu.sp == base);

		memory = {};
		memory.fault_read = true;
		memory.fault_write = true;
		cpu = {};
		cpu.pc = pc;
		cpu.registers[2] = base;
		cpu.vectors[3] = vector_pattern();
		const auto before = cpu;
		const auto fault = test::immediate_other(variant.size_code,
			variant.operation, 19, 3, 2, 3, true);
		const auto result = arm64::Dispatcher{}.execute(cpu, memory, fault);
		test::require(result.reason == arm64::StopReason::memory_fault);
		test::require(result.write == !variant.load);
		test::require(cpu.registers == before.registers);
		test::require(cpu.vectors[3].bytes == before.vectors[3].bytes);
		test::require(cpu.pc == pc);
	}
}

void reserved_encodings_are_illegal()
{
	test::ProbeMemory memory;
	/* Mode 10 is not an ordinary SIMD&FP transfer encoding. */
	for (const auto &variant : test::vector_variants)
		for (unsigned imm9 = 0; imm9 < 512; ++imm9) {
			arm64::CpuState cpu{};
			cpu.pc = pc;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				test::immediate_other(variant.size_code,
					variant.operation, imm9, 2, 2, 3, true)).reason ==
				arm64::StopReason::illegal_instruction);
		}

	for (unsigned size_code = 1; size_code < 4; ++size_code)
		for (const unsigned operation : {2U, 3U}) {
			for (unsigned imm12 = 0; imm12 < 4096; ++imm12) {
				arm64::CpuState cpu{};
				cpu.pc = pc;
				test::require(arm64::Dispatcher{}.execute(cpu, memory,
					test::unsigned_immediate(size_code, operation,
						imm12, 2, 3, true)).reason ==
					arm64::StopReason::illegal_instruction);
			}
			for (unsigned mode = 0; mode < 4; ++mode)
				for (unsigned imm9 = 0; imm9 < 512; ++imm9) {
					arm64::CpuState cpu{};
					cpu.pc = pc;
					test::require(arm64::Dispatcher{}.execute(cpu, memory,
						test::immediate_other(size_code, operation,
							imm9, mode, 2, 3, true)).reason ==
						arm64::StopReason::illegal_instruction);
				}
		}

	for (const auto &variant : test::vector_variants) {
		for (const unsigned mode : {0U, 1U, 3U})
			for (const unsigned option : {2U, 3U, 6U, 7U}) {
				arm64::CpuState cpu{};
				cpu.pc = pc;
				const auto instruction = test::register_offset(variant.size_code,
					variant.operation, 4, option, false, mode, 2, 3, true);
				/* FEAT_LSFE uses mode 00 of the VR=1 scalar-atomic
				 * allocation. Its complete behavior is covered separately. */
				if ((instruction & 0x3f20'0c00U) == 0x3c20'0000U)
					continue;
				const auto result = arm64::Dispatcher{}.execute(
					cpu, memory, instruction);
				if (result.reason != arm64::StopReason::illegal_instruction)
					std::fprintf(stderr,
						"reserved SIMD register-offset 0x%08x returned %u\n",
						instruction, static_cast<unsigned>(result.reason));
				test::require(result.reason ==
					arm64::StopReason::illegal_instruction);
			}
		for (const unsigned option : {0U, 1U, 4U, 5U}) {
			arm64::CpuState cpu{};
			cpu.pc = pc;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				test::register_offset(variant.size_code,
					variant.operation, 4, option, false,
					2, 2, 3, true)).reason ==
				arm64::StopReason::illegal_instruction);
		}
	}
	test::require(memory.reads == 0 && memory.writes == 0);
}

void gadget_immediate_and_register_matrix()
{
	test::StaticMapping mapping{0xa0'0000, 61,
		"/private/tmp/uml-simd-ls.XXXXXX"};
	const auto map_base = mapping.guest() +
		5 * test::StaticMapping::page_size;
	for (const auto &variant : test::vector_variants) {
		for (const unsigned imm12 : {0U, 1U, 4095U}) {
			const auto address = map_base + imm12 * variant.size;
			arm64::CpuState cpu{};
			cpu.registers[2] = map_base;
			cpu.vectors[3] = vector_pattern();
			test::ProbeMemory payload;
			std::memcpy(mapping.at(address), payload.payload.data(),
				variant.size);
			test::require(mapping.execute(pc, test::unsigned_immediate(
				variant.size_code, variant.operation, imm12,
				2, 3, true), cpu) == gadgets::Event::system_call);
			if (variant.load)
				require_loaded_vector(cpu.vectors[3], payload, variant.size);
			else {
				const auto expected = vector_pattern();
				test::require(std::memcmp(mapping.at(address),
					expected.bytes.data(), variant.size) == 0);
			}
		}

		for (const unsigned mode : {0U, 1U, 3U})
			for (const unsigned imm9 : {0U, 1U, 255U, 256U, 511U}) {
				const auto adjusted = map_base + test::signed_imm9(imm9);
				const auto address = mode == 1 ? map_base : adjusted;
				arm64::CpuState cpu{};
				cpu.registers[2] = map_base;
				cpu.vectors[3] = vector_pattern();
				test::ProbeMemory payload;
				std::memcpy(mapping.at(address), payload.payload.data(),
					variant.size);
				test::require(mapping.execute(pc, test::immediate_other(
					variant.size_code, variant.operation, imm9,
					mode, 2, 3, true), cpu) ==
					gadgets::Event::system_call);
				if (variant.load)
					require_loaded_vector(cpu.vectors[3], payload,
						variant.size);
				test::require(cpu.registers[2] ==
					(mode == 1 || mode == 3 ? adjusted : map_base));
			}

		for (const unsigned option : {2U, 3U, 6U, 7U})
			for (const bool scaled : {false, true}) {
				const auto raw = option >= 6 ? ~std::uint64_t{0} : 3U;
				const unsigned scale = variant.size == 16 ? 4 :
					variant.size_code;
				const auto address = map_base + test::extend_offset(
					raw, option, scaled ? scale : 0);
				arm64::CpuState cpu{};
				cpu.registers[2] = map_base;
				cpu.registers[4] = raw;
				cpu.vectors[3] = vector_pattern();
				test::ProbeMemory payload;
				std::memcpy(mapping.at(address), payload.payload.data(),
					variant.size);
				test::require(mapping.execute(pc, test::register_offset(
					variant.size_code, variant.operation, 4, option,
					scaled, 2, 2, 3, true), cpu) ==
					gadgets::Event::system_call);
				if (variant.load)
					require_loaded_vector(cpu.vectors[3], payload,
						variant.size);
			}
	}
}

void gadget_cross_page_fault_and_illegal()
{
	test::StaticMapping mapping{0xc0'0000, 67,
		"/private/tmp/uml-simd-ls-edge.XXXXXX"};
	for (const auto &variant : test::vector_variants) {
		const auto cross = mapping.guest() +
			4 * test::StaticMapping::page_size -
			(variant.size > 1 ? variant.size / 2 : 0);
		arm64::CpuState cpu{};
		cpu.registers[2] = cross;
		cpu.vectors[3] = vector_pattern();
		test::ProbeMemory payload;
		std::memcpy(mapping.at(cross), payload.payload.data(), variant.size);
		test::require(mapping.execute(pc, test::immediate_other(
			variant.size_code, variant.operation, 0, 0, 2, 3, true),
			cpu) == gadgets::Event::system_call);
		if (variant.load)
			require_loaded_vector(cpu.vectors[3], payload, variant.size);

		const auto fault = mapping.guest() + test::StaticMapping::length + 32;
		cpu = {};
		cpu.registers[2] = fault;
		cpu.vectors[3] = vector_pattern();
		const auto before = cpu;
		test::require(mapping.execute(pc, test::immediate_other(
			variant.size_code, variant.operation, 7, 1, 2, 3, true),
			cpu) == gadgets::Event::memory_fault);
		test::require(cpu.pc == pc);
		test::require(cpu.registers == before.registers);
		test::require(cpu.vectors[3].bytes == before.vectors[3].bytes);

		cpu = {};
		test::require(mapping.execute(pc, test::immediate_other(
			variant.size_code, variant.operation, 0, 2, 2, 3, true),
			cpu) == gadgets::Event::illegal_instruction);
		test::require(cpu.pc == pc);
	}
}

} // namespace

int main()
{
	exhaustive_interpreter_immediates();
	exhaustive_interpreter_register_offsets();
	special_registers_and_faults();
	reserved_encodings_are_illegal();
	gadget_immediate_and_register_matrix();
	gadget_cross_page_fault_and_illegal();
}
