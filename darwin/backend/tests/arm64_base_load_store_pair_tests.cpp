// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_test_support.hpp"

#include <array>
#include <cstring>

namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0xd0'0000;
constexpr std::uint64_t base = 0x80'0000;
constexpr std::uint64_t first_value = 0xf123'4567'89ab'cdefULL;
constexpr std::uint64_t second_value = 0x8123'4567'0bad'f00dULL;

struct Variant final {
	unsigned operation;
	std::size_t size;
	bool load;
};

constexpr std::array variants{
	Variant{0, 4, false}, Variant{0, 4, true},
	Variant{1, 4, true},
	Variant{2, 8, false}, Variant{2, 8, true},
	Variant{3, 8, false}, Variant{3, 8, true},
};

class PairMemory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t address,
				std::span<std::byte> output) override
	{
		last_address = address;
		++reads;
		read_addresses[reads - 1] = address;
		if (fault_read == reads)
			return false;
		const auto &source = reads == 1 ? first : second;
		std::copy_n(source.begin(), output.size(), output.begin());
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
				 std::span<const std::byte> input) override
	{
		last_address = address;
		++writes;
		if (fault_write)
			return false;
		written.fill(std::byte{});
		std::copy(input.begin(), input.end(), written.begin());
		return true;
	}

	PairMemory()
	{
		std::memcpy(first.data(), &first_value, sizeof(first_value));
		std::memcpy(second.data(), &second_value, sizeof(second_value));
	}

	std::array<std::byte, 8> first{};
	std::array<std::byte, 8> second{};
	std::array<std::byte, 16> written{};
	std::array<std::uint64_t, 2> read_addresses{};
	std::uint64_t last_address{};
	unsigned reads{};
	unsigned writes{};
	unsigned fault_read{};
	bool fault_write{};
};

[[nodiscard]] std::uint64_t expected_load(
	const Variant &variant, std::uint64_t value)
{
	if (variant.operation == 1)
		return test::sign_extend(value & 0xffff'ffffU, 32);
	return variant.size == 4 ? value & 0xffff'ffffU : value;
}

[[nodiscard]] bool valid_mode(const Variant &variant, unsigned mode)
{
	return variant.operation != 1 || mode != 0;
}

void check_interpreted(const Variant &variant, unsigned mode, unsigned imm7)
{
	PairMemory memory;
	arm64::CpuState cpu{};
	test::fill_registers(cpu);
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[3] = first_value;
	cpu.registers[4] = second_value;
	const auto displacement = test::signed_imm7(imm7) *
		static_cast<std::int64_t>(variant.size);
	const auto adjusted = base + displacement;
	const auto address = mode == 1 ? base : adjusted;
	const bool writeback = mode == 1 || mode == 3;
	const auto instruction = test::pair(variant.operation, false,
		variant.load, mode, imm7, 2, 3, 4);
	test::require(arm64::Dispatcher{}.execute(cpu, memory, instruction).reason ==
		arm64::StopReason::continued);
	test::require(cpu.pc == pc + 4);
	test::require(cpu.registers[2] == (writeback ? adjusted : base));
	if (variant.load) {
		test::require(memory.reads == 2 && memory.writes == 0);
		test::require(memory.read_addresses[0] == address);
		test::require(memory.read_addresses[1] == address + variant.size);
		test::require(cpu.registers[3] ==
			expected_load(variant, first_value));
		test::require(cpu.registers[4] ==
			expected_load(variant, second_value));
	} else {
		test::require(memory.reads == 0 && memory.writes == 1);
		test::require(memory.last_address == address);
		test::require(std::memcmp(memory.written.data(), &first_value,
			variant.size) == 0);
		test::require(std::memcmp(memory.written.data() + variant.size,
			&second_value, variant.size) == 0);
	}
}

void exhaustive_interpreter_variants()
{
	for (const auto &variant : variants)
		for (unsigned mode = 0; mode < 4; ++mode)
			for (unsigned imm7 = 0; imm7 < 128; ++imm7) {
				if (valid_mode(variant, mode)) {
					check_interpreted(variant, mode, imm7);
				} else {
					PairMemory memory;
					arm64::CpuState cpu{};
					cpu.pc = pc;
					test::require(arm64::Dispatcher{}.execute(cpu, memory,
						test::pair(variant.operation, false,
							variant.load, mode, imm7, 2, 3, 4)).reason ==
						arm64::StopReason::illegal_instruction);
					test::require(memory.reads == 0 && memory.writes == 0);
				}
			}
}

void overlap_constraints_and_zero_registers()
{
	for (const auto &variant : variants) {
		for (const unsigned mode : {1U, 3U}) {
			PairMemory memory;
			arm64::CpuState cpu{};
			cpu.pc = pc;
			cpu.registers[5] = base;
			cpu.registers[6] = second_value;
			const auto instruction = test::pair(variant.operation, false,
				variant.load, mode, 5, 5, 5, 6);
			test::require(arm64::Dispatcher{}.execute(
				cpu, memory, instruction).reason ==
				arm64::StopReason::continued);
			if (variant.load) {
				/* WBSUPPRESS leaves the loaded first value in the base. */
				test::require(cpu.registers[5] ==
					expected_load(variant, first_value));
			} else {
				test::require(std::memcmp(memory.written.data(), &base,
					variant.size) == 0);
				test::require(cpu.registers[5] == base + 5 * variant.size);
			}
		}

		PairMemory memory;
		arm64::CpuState cpu{};
		cpu.pc = pc;
		cpu.sp = base;
		const auto registers = cpu.registers;
		const auto instruction = test::pair(variant.operation, false,
			variant.load, variant.operation == 1 ? 2 : 0,
			0, 31, 31, 30);
		test::require(arm64::Dispatcher{}.execute(cpu, memory, instruction).reason ==
			arm64::StopReason::continued);
		test::require(cpu.sp == base);
		if (variant.load) {
			test::require(cpu.registers[30] ==
				expected_load(variant, second_value));
			for (unsigned index = 0; index < 30; ++index)
				test::require(cpu.registers[index] == registers[index]);
		} else {
			std::array<std::byte, 8> zero{};
			test::require(std::memcmp(memory.written.data(), zero.data(),
				variant.size) == 0);
		}
	}
}

void duplicate_load_destinations_are_illegal()
{
	for (const auto &variant : variants) {
		if (!variant.load)
			continue;
		for (unsigned mode = 0; mode < 4; ++mode) {
			if (!valid_mode(variant, mode))
				continue;
			for (unsigned reg = 0; reg < 32; ++reg) {
				PairMemory memory;
				arm64::CpuState cpu{};
				cpu.pc = pc;
				test::require(arm64::Dispatcher{}.execute(cpu, memory,
					test::pair(variant.operation, false, true, mode,
						0, 2, reg, reg)).reason ==
					arm64::StopReason::illegal_instruction);
				test::require(cpu.pc == pc);
				test::require(memory.reads == 0);
			}
		}
	}
}

void interpreter_fault_atomicity()
{
	for (const auto &variant : variants) {
		for (const unsigned fault_read : {1U, 2U}) {
			if (!variant.load)
				continue;
			PairMemory memory;
			memory.fault_read = fault_read;
			arm64::CpuState cpu{};
			test::fill_registers(cpu);
			cpu.pc = pc;
			cpu.registers[2] = base;
			const auto before = cpu;
			const auto mode = variant.operation == 1 ? 3U : 1U;
			const auto result = arm64::Dispatcher{}.execute(cpu, memory,
				test::pair(variant.operation, false, true, mode,
					3, 2, 3, 4));
			test::require(result.reason == arm64::StopReason::memory_fault);
			test::require(cpu.registers == before.registers);
			test::require(cpu.pc == pc);
		}
		if (!variant.load) {
			PairMemory memory;
			memory.fault_write = true;
			arm64::CpuState cpu{};
			cpu.pc = pc;
			cpu.registers[2] = base;
			cpu.registers[3] = first_value;
			cpu.registers[4] = second_value;
			const auto before = cpu;
			const auto result = arm64::Dispatcher{}.execute(cpu, memory,
				test::pair(variant.operation, false, false, 3,
					3, 2, 3, 4));
			test::require(result.reason == arm64::StopReason::memory_fault);
			test::require(cpu.registers == before.registers);
			test::require(cpu.pc == pc);
		}
	}
}

void gadget_matrix_and_edges()
{
	test::StaticMapping mapping{0xe0'0000, 71,
		"/private/tmp/uml-base-pair.XXXXXX"};
	const auto map_base = mapping.guest() +
		6 * test::StaticMapping::page_size;
	for (const auto &variant : variants)
		for (unsigned mode = 0; mode < 4; ++mode) {
			if (!valid_mode(variant, mode))
				continue;
			for (const unsigned imm7 : {0U, 1U, 63U, 64U, 127U}) {
				const auto adjusted = map_base + test::signed_imm7(imm7) *
					static_cast<std::int64_t>(variant.size);
				const auto address = mode == 1 ? map_base : adjusted;
				arm64::CpuState cpu{};
				cpu.registers[2] = map_base;
				cpu.registers[3] = first_value;
				cpu.registers[4] = second_value;
				std::memcpy(mapping.at(address), &first_value, variant.size);
				std::memcpy(mapping.at(address + variant.size),
					&second_value, variant.size);
				test::require(mapping.execute(pc, test::pair(
					variant.operation, false, variant.load, mode,
					imm7, 2, 3, 4), cpu) == gadgets::Event::system_call);
				if (variant.load) {
					test::require(cpu.registers[3] ==
						expected_load(variant, first_value));
					test::require(cpu.registers[4] ==
						expected_load(variant, second_value));
				} else {
					test::require(std::memcmp(mapping.at(address), &first_value,
						variant.size) == 0);
					test::require(std::memcmp(mapping.at(address + variant.size),
						&second_value, variant.size) == 0);
				}
				test::require(cpu.registers[2] ==
					(mode == 1 || mode == 3 ? adjusted : map_base));
			}
		}

	for (const auto &variant : variants) {
		if (!variant.load)
			continue;
		arm64::CpuState cpu{};
		test::require(mapping.execute(pc, test::pair(variant.operation,
			false, true, variant.operation == 1 ? 2 : 0,
			0, 2, 3, 3), cpu) == gadgets::Event::illegal_instruction);
		test::require(cpu.pc == pc);
	}
}

void gadget_cross_page_and_fault_atomicity()
{
	test::StaticMapping mapping{0x100'0000, 73,
		"/private/tmp/uml-base-pair-edge.XXXXXX"};
	for (const auto &variant : variants) {
		const auto mode = variant.operation == 1 ? 2U : 0U;
		for (const auto address : {
			mapping.guest() + 4 * test::StaticMapping::page_size - variant.size,
			mapping.guest() + 6 * test::StaticMapping::page_size -
				variant.size / 2}) {
			arm64::CpuState cpu{};
			cpu.registers[2] = address;
			cpu.registers[3] = first_value;
			cpu.registers[4] = second_value;
			std::memcpy(mapping.at(address), &first_value, variant.size);
			std::memcpy(mapping.at(address + variant.size),
				&second_value, variant.size);
			test::require(mapping.execute(pc, test::pair(variant.operation,
				false, variant.load, mode, 0, 2, 3, 4), cpu) ==
				gadgets::Event::system_call);
			if (variant.load) {
				test::require(cpu.registers[3] ==
					expected_load(variant, first_value));
				test::require(cpu.registers[4] ==
					expected_load(variant, second_value));
			}
		}

		std::array<std::byte, 8> sentinel{};
		sentinel.fill(std::byte{0x5a});
		for (const auto fault_address : {
			mapping.guest() + test::StaticMapping::length - variant.size,
			mapping.guest() + test::StaticMapping::length - variant.size / 2}) {
			const auto inside = static_cast<std::size_t>(mapping.guest() +
				test::StaticMapping::length - fault_address);
			std::memcpy(mapping.at(fault_address), sentinel.data(), inside);
			arm64::CpuState cpu{};
			cpu.registers[2] = fault_address;
			cpu.registers[3] = first_value;
			cpu.registers[4] = second_value;
			const auto before = cpu.registers;
			test::require(mapping.execute(pc, test::pair(variant.operation,
				false, variant.load, mode, 0, 2, 3, 4), cpu) ==
				gadgets::Event::memory_fault);
			test::require(cpu.pc == pc);
			test::require(cpu.registers == before);
			if (!variant.load)
				test::require(std::memcmp(mapping.at(fault_address),
					sentinel.data(), inside) == 0);
		}
	}
}

} // namespace

int main()
{
	exhaustive_interpreter_variants();
	overlap_constraints_and_zero_registers();
	duplicate_load_destinations_are_illegal();
	interpreter_fault_atomicity();
	gadget_matrix_and_edges();
	gadget_cross_page_and_fault_atomicity();
}
