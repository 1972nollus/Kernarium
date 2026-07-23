// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_test_support.hpp"

#include <array>
#include <cstring>

namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0xf0'0000;
constexpr std::uint64_t base = 0x90'0000;

struct Variant final {
	unsigned operation;
	std::size_t size;
	bool load;
};

constexpr std::array variants{
	Variant{0, 4, false}, Variant{0, 4, true},
	Variant{1, 8, false}, Variant{1, 8, true},
	Variant{2, 16, false}, Variant{2, 16, true},
	Variant{3, 16, false}, Variant{3, 16, true},
};

[[nodiscard]] arm64::VectorRegister pattern(unsigned seed)
{
	arm64::VectorRegister value;
	for (unsigned index = 0; index < value.bytes.size(); ++index)
		value.bytes[index] = static_cast<std::byte>(seed + index * 17U);
	return value;
}

class PairMemory final : public arm64::GuestMemory {
public:
	PairMemory() : first{pattern(0x21).bytes}, second{pattern(0x92).bytes} {}

	[[nodiscard]] bool read(std::uint64_t address,
				std::span<std::byte> output) override
	{
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
		++writes;
		write_address = address;
		if (fault_write)
			return false;
		written.fill(std::byte{});
		std::copy(input.begin(), input.end(), written.begin());
		return true;
	}

	std::array<std::byte, 16> first{};
	std::array<std::byte, 16> second{};
	std::array<std::byte, 32> written{};
	std::array<std::uint64_t, 2> read_addresses{};
	std::uint64_t write_address{};
	unsigned reads{};
	unsigned writes{};
	unsigned fault_read{};
	bool fault_write{};
};

void require_loaded(const arm64::VectorRegister &actual,
	const std::array<std::byte, 16> &source, std::size_t size)
{
	for (std::size_t index = 0; index < actual.bytes.size(); ++index)
		test::require(actual.bytes[index] ==
			(index < size ? source[index] : std::byte{}));
}

void check_interpreted(const Variant &variant, unsigned mode, unsigned imm7)
{
	PairMemory memory;
	arm64::CpuState cpu{};
	test::fill_registers(cpu);
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.vectors[3] = pattern(0x43);
	cpu.vectors[4] = pattern(0x74);
	const auto first_source = cpu.vectors[3];
	const auto second_source = cpu.vectors[4];
	const auto displacement = test::signed_imm7(imm7) *
		static_cast<std::int64_t>(variant.size);
	const auto adjusted = base + displacement;
	const auto address = mode == 1 ? base : adjusted;
	const bool writeback = mode == 1 || mode == 3;
	const auto instruction = test::pair(variant.operation, true,
		variant.load, mode, imm7, 2, 3, 4);
	test::require(arm64::Dispatcher{}.execute(cpu, memory, instruction).reason ==
		arm64::StopReason::continued);
	test::require(cpu.pc == pc + 4);
	test::require(cpu.registers[2] == (writeback ? adjusted : base));
	if (variant.load) {
		test::require(memory.reads == 2 && memory.writes == 0);
		test::require(memory.read_addresses[0] == address);
		test::require(memory.read_addresses[1] == address + variant.size);
		require_loaded(cpu.vectors[3], memory.first, variant.size);
		require_loaded(cpu.vectors[4], memory.second, variant.size);
	} else {
		test::require(memory.reads == 0 && memory.writes == 1);
		test::require(memory.write_address == address);
		test::require(std::memcmp(memory.written.data(),
			first_source.bytes.data(), variant.size) == 0);
		test::require(std::memcmp(memory.written.data() + variant.size,
			second_source.bytes.data(), variant.size) == 0);
	}
}

void exhaustive_interpreter_variants()
{
	for (const auto &variant : variants)
		for (unsigned mode = 0; mode < 4; ++mode)
			for (unsigned imm7 = 0; imm7 < 128; ++imm7)
				check_interpreted(variant, mode, imm7);
}

void vector_31_and_duplicate_load_constraints()
{
	for (const auto &variant : variants) {
		PairMemory memory;
		arm64::CpuState cpu{};
		cpu.pc = pc;
		cpu.sp = base;
		cpu.vectors[31] = pattern(0x35);
		cpu.vectors[30] = pattern(0xa3);
		const auto first_source = cpu.vectors[31];
		const auto second_source = cpu.vectors[30];
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			test::pair(variant.operation, true, variant.load,
				3, 1, 31, 31, 30)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.sp == base + variant.size);
		if (variant.load) {
			require_loaded(cpu.vectors[31], memory.first, variant.size);
			require_loaded(cpu.vectors[30], memory.second, variant.size);
		} else {
			test::require(std::memcmp(memory.written.data(),
				first_source.bytes.data(), variant.size) == 0);
			test::require(std::memcmp(memory.written.data() + variant.size,
				second_source.bytes.data(), variant.size) == 0);
		}

		if (!variant.load)
			continue;
		for (unsigned mode = 0; mode < 4; ++mode)
			for (unsigned reg = 0; reg < 32; ++reg) {
				memory = {};
				cpu = {};
				cpu.pc = pc;
				test::require(arm64::Dispatcher{}.execute(cpu, memory,
					test::pair(variant.operation, true, true,
						mode, 0, 2, reg, reg)).reason ==
					arm64::StopReason::illegal_instruction);
				test::require(cpu.pc == pc && memory.reads == 0);
			}
	}
}

void interpreter_fault_atomicity()
{
	for (const auto &variant : variants) {
		if (variant.load) {
			for (const unsigned fault_read : {1U, 2U}) {
				PairMemory memory;
				memory.fault_read = fault_read;
				arm64::CpuState cpu{};
				test::fill_registers(cpu);
				cpu.pc = pc;
				cpu.registers[2] = base;
				cpu.vectors[3] = pattern(0x44);
				cpu.vectors[4] = pattern(0x55);
				const auto before = cpu;
				const auto result = arm64::Dispatcher{}.execute(cpu, memory,
					test::pair(variant.operation, true, true,
						3, 3, 2, 3, 4));
				test::require(result.reason == arm64::StopReason::memory_fault);
				test::require(cpu.registers == before.registers);
				test::require(cpu.vectors[3].bytes == before.vectors[3].bytes);
				test::require(cpu.vectors[4].bytes == before.vectors[4].bytes);
				test::require(cpu.pc == pc);
			}
		} else {
			PairMemory memory;
			memory.fault_write = true;
			arm64::CpuState cpu{};
			test::fill_registers(cpu);
			cpu.pc = pc;
			cpu.registers[2] = base;
			cpu.vectors[3] = pattern(0x44);
			cpu.vectors[4] = pattern(0x55);
			const auto before = cpu;
			const auto result = arm64::Dispatcher{}.execute(cpu, memory,
				test::pair(variant.operation, true, false,
					3, 3, 2, 3, 4));
			test::require(result.reason == arm64::StopReason::memory_fault);
			test::require(cpu.registers == before.registers);
			test::require(cpu.vectors[3].bytes == before.vectors[3].bytes);
			test::require(cpu.vectors[4].bytes == before.vectors[4].bytes);
			test::require(cpu.pc == pc);
		}
	}
}

void gadget_matrix()
{
	test::StaticMapping mapping{0x110'0000, 79,
		"/private/tmp/uml-simd-pair.XXXXXX"};
	const auto map_base = mapping.guest() +
		6 * test::StaticMapping::page_size;
	for (const auto &variant : variants)
		for (unsigned mode = 0; mode < 4; ++mode)
			for (const unsigned imm7 : {0U, 1U, 63U, 64U, 127U}) {
				const auto adjusted = map_base + test::signed_imm7(imm7) *
					static_cast<std::int64_t>(variant.size);
				const auto address = mode == 1 ? map_base : adjusted;
				PairMemory payload;
				arm64::CpuState cpu{};
				cpu.registers[2] = map_base;
				cpu.vectors[3] = pattern(0x43);
				cpu.vectors[4] = pattern(0x74);
				const auto first_source = cpu.vectors[3];
				const auto second_source = cpu.vectors[4];
				if (variant.load) {
					std::memcpy(mapping.at(address), payload.first.data(),
						variant.size);
					std::memcpy(mapping.at(address + variant.size),
						payload.second.data(), variant.size);
				}
				test::require(mapping.execute(pc, test::pair(
					variant.operation, true, variant.load, mode,
					imm7, 2, 3, 4), cpu) == gadgets::Event::system_call);
				if (variant.load) {
					require_loaded(cpu.vectors[3], payload.first, variant.size);
					require_loaded(cpu.vectors[4], payload.second, variant.size);
				} else {
					test::require(std::memcmp(mapping.at(address),
						first_source.bytes.data(), variant.size) == 0);
					test::require(std::memcmp(mapping.at(address + variant.size),
						second_source.bytes.data(), variant.size) == 0);
				}
				test::require(cpu.registers[2] ==
					(mode == 1 || mode == 3 ? adjusted : map_base));
			}

	for (const auto &variant : variants) {
		if (!variant.load)
			continue;
		arm64::CpuState cpu{};
		test::require(mapping.execute(pc, test::pair(variant.operation,
			true, true, 0, 0, 2, 3, 3), cpu) ==
			gadgets::Event::illegal_instruction);
		test::require(cpu.pc == pc);
	}
}

void gadget_page_edges_and_fault_atomicity()
{
	test::StaticMapping mapping{0x130'0000, 83,
		"/private/tmp/uml-simd-pair-edge.XXXXXX"};
	for (const auto &variant : variants) {
		PairMemory payload;
		for (const auto address : {
			mapping.guest() + 4 * test::StaticMapping::page_size - variant.size,
			mapping.guest() + 6 * test::StaticMapping::page_size -
				variant.size / 2}) {
			arm64::CpuState cpu{};
			cpu.registers[2] = address;
			cpu.vectors[3] = pattern(0x43);
			cpu.vectors[4] = pattern(0x74);
			const auto first_source = cpu.vectors[3];
			const auto second_source = cpu.vectors[4];
			if (variant.load) {
				std::memcpy(mapping.at(address), payload.first.data(),
					variant.size);
				std::memcpy(mapping.at(address + variant.size),
					payload.second.data(), variant.size);
			}
			test::require(mapping.execute(pc, test::pair(variant.operation,
				true, variant.load, 0, 0, 2, 3, 4), cpu) ==
				gadgets::Event::system_call);
			if (variant.load) {
				require_loaded(cpu.vectors[3], payload.first, variant.size);
				require_loaded(cpu.vectors[4], payload.second, variant.size);
			} else {
				test::require(std::memcmp(mapping.at(address),
					first_source.bytes.data(), variant.size) == 0);
				test::require(std::memcmp(mapping.at(address + variant.size),
					second_source.bytes.data(), variant.size) == 0);
			}
		}

		std::array<std::byte, 16> sentinel{};
		sentinel.fill(std::byte{0x5a});
		for (const auto fault_address : {
			mapping.guest() + test::StaticMapping::length - variant.size,
			mapping.guest() + test::StaticMapping::length - variant.size / 2}) {
			const auto inside = static_cast<std::size_t>(
				mapping.guest() + test::StaticMapping::length - fault_address);
			std::memcpy(mapping.at(fault_address), sentinel.data(), inside);
			arm64::CpuState cpu{};
			cpu.registers[2] = fault_address;
			cpu.vectors[3] = pattern(0x43);
			cpu.vectors[4] = pattern(0x74);
			const auto before = cpu;
			test::require(mapping.execute(pc, test::pair(variant.operation,
				true, variant.load, 0, 0, 2, 3, 4), cpu) ==
				gadgets::Event::memory_fault);
			test::require(cpu.pc == pc);
			test::require(cpu.registers == before.registers);
			test::require(cpu.vectors[3].bytes == before.vectors[3].bytes);
			test::require(cpu.vectors[4].bytes == before.vectors[4].bytes);
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
	vector_31_and_duplicate_load_constraints();
	interpreter_fault_atomicity();
	gadget_matrix();
	gadget_page_edges_and_fault_atomicity();
}
