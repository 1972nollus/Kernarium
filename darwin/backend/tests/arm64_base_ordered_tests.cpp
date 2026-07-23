// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_test_support.hpp"

#include <array>
#include <cstring>

namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x270'0000;
constexpr std::uint64_t base = 0xb0'0040;
constexpr std::uint64_t value = 0xf1e2'd3c4'b5a6'9788ULL;

[[nodiscard]] constexpr std::uint32_t ordered(unsigned size_code, bool load,
	bool acquire_release, unsigned rn, unsigned rt, unsigned status = 31,
	unsigned rt2 = 31, bool pair = false)
{
	return 0x0800'0000U | ((size_code & 3U) << 30) |
		(static_cast<unsigned>(pair) << 21) |
		(static_cast<unsigned>(load) << 22) | (1U << 23) |
		((status & 31U) << 16) |
		(static_cast<unsigned>(acquire_release) << 15) |
		((rt2 & 31U) << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t ordered_writeback(
	unsigned size_code, bool load, unsigned rn, unsigned rt)
{
	return 0x1980'0800U | ((size_code & 3U) << 30) |
		(static_cast<unsigned>(load) << 22) | ((rn & 31U) << 5) |
		(rt & 31U);
}

class alignas(64) AtomicMemory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t address,
				std::span<std::byte> output) override
	{
		++reads;
		if (fault || !range_valid(address, output.size()))
			return false;
		std::copy_n(pointer(address), output.size(), output.begin());
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
				 std::span<const std::byte> input) override
	{
		++writes;
		if (fault || !range_valid(address, input.size()))
			return false;
		std::copy(input.begin(), input.end(), pointer(address));
		return true;
	}

	[[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
		std::size_t size, arm64::MemoryAccess) override
	{
		++atomic_pointers;
		return fault || !range_valid(address, size) ? nullptr : pointer(address);
	}

	void put(std::uint64_t address, std::uint64_t source, std::size_t size)
	{
		std::memcpy(pointer(address), &source, size);
	}

	[[nodiscard]] std::uint64_t get(std::uint64_t address,
		std::size_t size) const
	{
		std::uint64_t result{};
		std::memcpy(&result, pointer(address), size);
		return result;
	}

	bool fault{};
	unsigned reads{};
	unsigned writes{};
	unsigned atomic_pointers{};

private:
	[[nodiscard]] bool range_valid(std::uint64_t address,
		std::size_t size) const
	{
		return address >= base - 32 &&
			address - (base - 32) <= bytes.size() &&
			size <= bytes.size() - (address - (base - 32));
	}

	[[nodiscard]] std::byte *pointer(std::uint64_t address)
	{
		return bytes.data() + (address - (base - 32));
	}

	[[nodiscard]] const std::byte *pointer(std::uint64_t address) const
	{
		return bytes.data() + (address - (base - 32));
	}

	alignas(64) std::array<std::byte, 128> bytes{};
};

[[nodiscard]] std::uint64_t size_mask(std::size_t size)
{
	return size == 8 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << (size * 8)) - 1;
}

void exhaustive_interpreter_no_offset()
{
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		for (const bool acquire_release : {false, true})
			for (const bool load : {false, true}) {
				AtomicMemory memory;
				memory.put(base, value, size);
				arm64::CpuState cpu{};
				test::fill_registers(cpu);
				cpu.pc = pc;
				cpu.registers[2] = base;
				cpu.registers[3] = value;
				const auto before = cpu;
				const auto result = arm64::Dispatcher{}.execute(cpu, memory,
					ordered(size_code, load, acquire_release, 2, 3));
				test::require(result.reason == arm64::StopReason::continued);
				test::require(cpu.pc == pc + 4);
				if (load) {
					test::require(cpu.registers[3] == (value & size_mask(size)));
				} else {
					test::require(memory.get(base, size) ==
						(value & size_mask(size)));
					test::require(cpu.registers == before.registers);
				}
			}
	}
}

void zero_register_sp_faults_and_alignment()
{
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		for (const bool load : {false, true}) {
			AtomicMemory memory;
			memory.put(base, value, size);
			arm64::CpuState cpu{};
			cpu.pc = pc;
			cpu.sp = base;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				ordered(size_code, load, true, 31, 31)).reason ==
				arm64::StopReason::continued);
			test::require(cpu.sp == base);
			if (!load)
				test::require(memory.get(base, size) == 0);

			memory = {};
			memory.fault = true;
			cpu = {};
			cpu.pc = pc;
			cpu.registers[2] = base;
			cpu.registers[3] = value;
			const auto before = cpu;
			const auto fault = arm64::Dispatcher{}.execute(cpu, memory,
				ordered(size_code, load, true, 2, 3));
			test::require(fault.reason == arm64::StopReason::memory_fault);
			test::require(fault.address == base && fault.write == !load);
			test::require(cpu.pc == pc && cpu.registers == before.registers);
		}

		if (size == 1)
			continue;
		for (const bool load : {false, true}) {
			AtomicMemory memory;
			arm64::CpuState cpu{};
			cpu.pc = pc;
			cpu.registers[2] = base + 1;
			cpu.registers[3] = value;
			const auto before = cpu;
			const auto fault = arm64::Dispatcher{}.execute(cpu, memory,
				ordered(size_code, load, true, 2, 3));
			test::require(fault.reason == arm64::StopReason::memory_fault);
			test::require(fault.address == base + 1 && fault.write == !load);
			test::require(memory.atomic_pointers == 0);
			test::require(cpu.registers == before.registers && cpu.pc == pc);
		}
	}
}

void interpreter_writeback_forms()
{
	for (unsigned size_code = 2; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		for (const bool load : {false, true}) {
			AtomicMemory memory;
			memory.put(load ? base : base - size, value, size);
			arm64::CpuState cpu{};
			cpu.pc = pc;
			cpu.registers[2] = base;
			cpu.registers[3] = value;
			const auto result = arm64::Dispatcher{}.execute(cpu, memory,
				ordered_writeback(size_code, load, 2, 3));
			test::require(result.reason == arm64::StopReason::continued);
			test::require(cpu.pc == pc + 4);
			test::require(cpu.registers[2] == (load ? base + size : base - size));
			if (load)
				test::require(cpu.registers[3] == (value & size_mask(size)));
			else
				test::require(memory.get(base - size, size) ==
					(value & size_mask(size)));
		}

		/* WBSUPPRESS for LDAPR, original source value plus writeback for STLR. */
		for (const bool load : {false, true}) {
			AtomicMemory memory;
			memory.put(load ? base : base - size, value, size);
			arm64::CpuState cpu{};
			cpu.pc = pc;
			cpu.registers[2] = base;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				ordered_writeback(size_code, load, 2, 2)).reason ==
				arm64::StopReason::continued);
			if (load) {
				test::require(cpu.registers[2] == (value & size_mask(size)));
			} else {
				test::require(memory.get(base - size, size) ==
					(base & size_mask(size)));
				test::require(cpu.registers[2] == base - size);
			}
		}

		for (const bool load : {false, true}) {
			AtomicMemory memory;
			memory.fault = true;
			arm64::CpuState cpu{};
			cpu.pc = pc;
			cpu.registers[2] = base;
			cpu.registers[3] = value;
			const auto before = cpu;
			const auto fault = arm64::Dispatcher{}.execute(cpu, memory,
				ordered_writeback(size_code, load, 2, 3));
			test::require(fault.reason == arm64::StopReason::memory_fault);
			test::require(fault.address == (load ? base : base - size));
			test::require(fault.write == !load && cpu.pc == pc);
			test::require(cpu.registers == before.registers);
		}
	}
}

void reserved_encodings_are_illegal()
{
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		for (const bool load : {false, true}) {
			for (const auto instruction : {
				ordered(size_code, load, true, 2, 3, 30, 31),
				ordered(size_code, load, true, 2, 3, 31, 30)}) {
				AtomicMemory memory;
				arm64::CpuState cpu{};
				cpu.pc = pc;
				test::require(arm64::Dispatcher{}.execute(cpu, memory,
					instruction).reason == arm64::StopReason::illegal_instruction);
			}
		}
		if (size_code < 2) {
			for (const bool load : {false, true}) {
				AtomicMemory memory;
				arm64::CpuState cpu{};
				cpu.pc = pc;
				test::require(arm64::Dispatcher{}.execute(cpu, memory,
					ordered_writeback(size_code, load, 2, 3)).reason ==
					arm64::StopReason::illegal_instruction);
			}
		}
	}
}

void gadget_no_offset_and_writeback_matrix()
{
	test::StaticMapping mapping{0x290'0000, 113,
		"/private/tmp/uml-base-ordered.XXXXXX"};
	const auto map_base = mapping.guest() +
		4 * test::StaticMapping::page_size;
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		for (const bool acquire_release : {false, true})
			for (const bool load : {false, true}) {
				std::memcpy(mapping.at(map_base), &value, size);
				arm64::CpuState cpu{};
				cpu.registers[2] = map_base;
				cpu.registers[3] = value;
				test::require(mapping.execute(pc, ordered(size_code, load,
					acquire_release, 2, 3), cpu) == gadgets::Event::system_call);
				if (load)
					test::require(cpu.registers[3] ==
						(value & size_mask(size)));
				else {
					std::uint64_t actual{};
					std::memcpy(&actual, mapping.at(map_base), size);
					test::require(actual == (value & size_mask(size)));
				}
			}
	}

	for (unsigned size_code = 2; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		for (const bool load : {false, true}) {
			std::memcpy(mapping.at(load ? map_base : map_base - size),
				&value, size);
			arm64::CpuState cpu{};
			cpu.registers[2] = map_base;
			cpu.registers[3] = value;
			test::require(mapping.execute(pc,
				ordered_writeback(size_code, load, 2, 3), cpu) ==
				gadgets::Event::system_call);
			test::require(cpu.registers[2] ==
				(load ? map_base + size : map_base - size));
		}
	}
}

void gadget_faults_alignment_overlap_and_illegal()
{
	test::StaticMapping mapping{0x2b0'0000, 127,
		"/private/tmp/uml-base-ordered-edge.XXXXXX"};
	const auto map_base = mapping.guest() +
		4 * test::StaticMapping::page_size;
	for (unsigned size_code = 1; size_code < 4; ++size_code)
		for (const bool load : {false, true}) {
			arm64::CpuState cpu{};
			cpu.registers[2] = map_base + 1;
			cpu.registers[3] = value;
			const auto before = cpu.registers;
			test::require(mapping.execute(pc, ordered(size_code, load,
				true, 2, 3), cpu) == gadgets::Event::memory_fault);
			test::require(cpu.pc == pc && cpu.registers == before);
		}

	for (unsigned size_code = 2; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		for (const bool load : {false, true}) {
			std::memcpy(mapping.at(load ? map_base : map_base - size),
				&value, size);
			arm64::CpuState cpu{};
			cpu.registers[2] = map_base;
			test::require(mapping.execute(pc,
				ordered_writeback(size_code, load, 2, 2), cpu) ==
				gadgets::Event::system_call);
			if (load)
				test::require(cpu.registers[2] ==
					(value & size_mask(size)));
			else {
				std::uint64_t actual{};
				std::memcpy(&actual, mapping.at(map_base - size), size);
				test::require(actual == (map_base & size_mask(size)));
				test::require(cpu.registers[2] == map_base - size);
			}
		}
	}

	arm64::CpuState cpu{};
	test::require(mapping.execute(pc, ordered(3, true, true, 2, 3,
		30, 31), cpu) == gadgets::Event::illegal_instruction);
	test::require(mapping.execute(pc, ordered(3, true, true, 2, 3,
		31, 30), cpu) == gadgets::Event::illegal_instruction);
	test::require(mapping.execute(pc, ordered_writeback(1, false, 2, 3),
		cpu) == gadgets::Event::illegal_instruction);
}

} // namespace

int main()
{
	exhaustive_interpreter_no_offset();
	zero_register_sp_faults_and_alignment();
	interpreter_writeback_forms();
	reserved_encodings_are_illegal();
	gadget_no_offset_and_writeback_matrix();
	gadget_faults_alignment_overlap_and_illegal();
}
