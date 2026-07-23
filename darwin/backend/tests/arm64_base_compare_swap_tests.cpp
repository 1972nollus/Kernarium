// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_test_support.hpp"

#include <array>
#include <cstring>

namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x370'0000;
constexpr std::uint64_t base = 0xe0'0080;
constexpr std::uint64_t initial_value = 0xf1e2'd3c4'b5a6'9788ULL;
constexpr std::uint64_t replacement_value = 0x1021'3243'5465'7687ULL;

[[nodiscard]] constexpr std::uint32_t compare_swap(unsigned size_code,
	bool acquire, bool release, unsigned rn, unsigned rs, unsigned rt,
	unsigned rt2 = 31)
{
	return 0x08a0'0000U | ((size_code & 3U) << 30) |
		(static_cast<unsigned>(acquire) << 22) | ((rs & 31U) << 16) |
		(static_cast<unsigned>(release) << 15) | ((rt2 & 31U) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t unprivileged_compare_swap(bool wide,
	bool acquire, bool release, unsigned rn, unsigned rs, unsigned rt,
	unsigned rt2 = 31)
{
	return 0x8980'0000U | (static_cast<unsigned>(wide) << 30) |
		(static_cast<unsigned>(acquire) << 22) | ((rs & 31U) << 16) |
		(static_cast<unsigned>(release) << 15) | ((rt2 & 31U) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t scalar_exclusive(unsigned size_code,
	bool load, unsigned rn, unsigned rt, unsigned status = 31)
{
	return 0x0800'0000U | ((size_code & 3U) << 30) |
		(static_cast<unsigned>(load) << 22) | ((status & 31U) << 16) |
		(31U << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint64_t size_mask(std::size_t size)
{
	return size == 8 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << (size * 8)) - 1;
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
		return direct && !fault && range_valid(address, size) ?
			pointer(address) : nullptr;
	}

	void put(std::uint64_t address, std::uint64_t value, std::size_t size)
	{
		std::memcpy(pointer(address), &value, size);
	}

	[[nodiscard]] std::uint64_t get(std::uint64_t address,
		std::size_t size) const
	{
		std::uint64_t result{};
		std::memcpy(&result, pointer(address), size);
		return result;
	}

	bool direct{true};
	bool fault{};
	unsigned reads{};
	unsigned writes{};
	unsigned atomic_pointers{};

private:
	[[nodiscard]] bool range_valid(std::uint64_t address,
		std::size_t size) const
	{
		return address >= base - 64 &&
			address - (base - 64) <= bytes.size() &&
			size <= bytes.size() - (address - (base - 64));
	}

	[[nodiscard]] std::byte *pointer(std::uint64_t address)
	{
		return bytes.data() + (address - (base - 64));
	}

	[[nodiscard]] const std::byte *pointer(std::uint64_t address) const
	{
		return bytes.data() + (address - (base - 64));
	}

	alignas(64) std::array<std::byte, 256> bytes{};
};

void run_interpreter_variant(std::uint32_t instruction, std::size_t size,
	bool direct)
{
	const auto mask = size_mask(size);
	AtomicMemory memory;
	memory.direct = direct;
	memory.put(base, initial_value, size);
	arm64::CpuState cpu{};
	test::fill_registers(cpu);
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[3] = initial_value;
	cpu.registers[4] = replacement_value;
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		instruction).reason == arm64::StopReason::continued);
	test::require(cpu.registers[3] == (initial_value & mask));
	test::require(memory.get(base, size) == (replacement_value & mask));
	test::require(cpu.pc == pc + 4);

	/* Compare failure returns the observed value, zero-extended to the
	 * architectural register width, and leaves memory unchanged. */
	memory.put(base, initial_value, size);
	cpu = {};
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[3] = initial_value ^ 1U;
	cpu.registers[4] = replacement_value;
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		instruction).reason == arm64::StopReason::continued);
	test::require(cpu.registers[3] == (initial_value & mask));
	test::require(memory.get(base, size) == (initial_value & mask));
}

void exhaustive_interpreter_variants()
{
	for (const bool direct : {false, true}) {
		for (unsigned size_code = 0; size_code < 4; ++size_code) {
			const auto size = std::size_t{1} << size_code;
			for (const bool acquire : {false, true})
				for (const bool release : {false, true})
					run_interpreter_variant(compare_swap(size_code,
						acquire, release, 2, 3, 4), size, direct);
		}
		for (const bool acquire : {false, true})
			for (const bool release : {false, true})
				run_interpreter_variant(unprivileged_compare_swap(true,
					acquire, release, 2, 3, 4), 8, direct);
	}
}

void interpreter_register_edges_and_reservations()
{
	AtomicMemory memory;
	arm64::CpuState cpu{};
	cpu.pc = pc;
	cpu.sp = base;
	cpu.registers[4] = replacement_value;
	memory.put(base, 0, 8);
	/* Rs==XZR compares against zero and discards the observed value. */
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		compare_swap(3, true, false, 31, 31, 4)).reason ==
		arm64::StopReason::continued);
	test::require(memory.get(base, 8) == replacement_value && cpu.sp == base);

	/* Rt==XZR conditionally stores zero. */
	cpu.registers[3] = replacement_value;
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		compare_swap(3, false, true, 31, 3, 31)).reason ==
		arm64::StopReason::continued);
	test::require(memory.get(base, 8) == 0 &&
		cpu.registers[3] == replacement_value);

	/* Rs==Rt captures both input roles before writing the observed result. */
	memory.put(base, initial_value, 8);
	cpu.registers[3] = initial_value;
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		compare_swap(3, false, false, 31, 3, 3)).reason ==
		arm64::StopReason::continued);
	test::require(memory.get(base, 8) == initial_value &&
		cpu.registers[3] == initial_value);

	/* The base address is captured before an overlapping Rs is updated. */
	memory.put(base, base, 8);
	cpu = {};
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[4] = replacement_value;
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		compare_swap(3, false, false, 2, 2, 4)).reason ==
		arm64::StopReason::continued);
	test::require(cpu.registers[2] == base &&
		memory.get(base, 8) == replacement_value);

	/* This implementation chooses the permitted behavior in which even a
	 * failed CAS read invalidates an exclusive reservation. */
	memory.put(base, initial_value, 8);
	cpu = {};
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[3] = replacement_value;
	cpu.registers[4] = replacement_value;
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		scalar_exclusive(3, true, 2, 5)).reason ==
		arm64::StopReason::continued);
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		compare_swap(3, false, false, 2, 3, 4)).reason ==
		arm64::StopReason::continued);
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		scalar_exclusive(3, false, 2, 4, 6)).reason ==
		arm64::StopReason::continued);
	test::require(cpu.registers[6] == 1 && memory.get(base, 8) == initial_value);
}

void interpreter_fault_alignment_and_reserved()
{
	for (unsigned size_code = 1; size_code < 4; ++size_code) {
		AtomicMemory memory;
		arm64::CpuState cpu{};
		cpu.pc = pc;
		cpu.registers[2] = base + 1;
		cpu.registers[3] = initial_value;
		cpu.registers[4] = replacement_value;
		const auto before = cpu;
		const auto result = arm64::Dispatcher{}.execute(cpu, memory,
			compare_swap(size_code, true, true, 2, 3, 4));
		test::require(result.reason == arm64::StopReason::memory_fault &&
			result.address == base + 1 && result.write);
		test::require(cpu.pc == pc && cpu.registers == before.registers &&
			memory.atomic_pointers == 0);
	}

	AtomicMemory memory;
	memory.fault = true;
	arm64::CpuState cpu{};
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[3] = initial_value;
	cpu.registers[4] = replacement_value;
	const auto before = cpu;
	const auto fault = arm64::Dispatcher{}.execute(cpu, memory,
		compare_swap(3, true, true, 2, 3, 4));
	test::require(fault.reason == arm64::StopReason::memory_fault &&
		fault.address == base && fault.write);
	test::require(cpu.pc == pc && cpu.registers == before.registers);

	for (const auto instruction : {
		compare_swap(3, false, false, 2, 3, 4, 30),
		unprivileged_compare_swap(true, false, false, 2, 3, 4, 30),
		unprivileged_compare_swap(false, false, false, 2, 3, 4)}) {
		memory = {};
		cpu = {};
		cpu.pc = pc;
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			instruction).reason == arm64::StopReason::illegal_instruction);
		test::require(cpu.pc == pc && memory.atomic_pointers == 0);
	}
}

void run_gadget_variant(test::StaticMapping &mapping, std::uint64_t map_base,
	std::uint32_t instruction, std::size_t size)
{
	const auto mask = size_mask(size);
	std::memcpy(mapping.at(map_base), &initial_value, size);
	arm64::CpuState cpu{};
	cpu.registers[2] = map_base;
	cpu.registers[3] = initial_value;
	cpu.registers[4] = replacement_value;
	test::require(mapping.execute(pc, instruction, cpu) ==
		gadgets::Event::system_call);
	test::require(cpu.registers[3] == (initial_value & mask));
	std::uint64_t actual{};
	std::memcpy(&actual, mapping.at(map_base), size);
	test::require(actual == (replacement_value & mask));

	std::memcpy(mapping.at(map_base), &initial_value, size);
	cpu.registers[3] = initial_value ^ 1U;
	cpu.registers[4] = replacement_value;
	test::require(mapping.execute(pc, instruction, cpu) ==
		gadgets::Event::system_call);
	test::require(cpu.registers[3] == (initial_value & mask));
	std::memcpy(&actual, mapping.at(map_base), size);
	test::require(actual == (initial_value & mask));
}

void exhaustive_gadget_variants_and_edges()
{
	test::StaticMapping mapping{0x390'0000, 173,
		"/private/tmp/uml-base-compare-swap.XXXXXX"};
	const auto map_base = mapping.guest() +
		4 * test::StaticMapping::page_size;
	arm64::CpuState warm{};
	warm.registers[2] = map_base;
	warm.registers[3] = initial_value;
	test::require(mapping.execute(pc,
		test::unsigned_immediate(3, 0, 0, 2, 3), warm) ==
		gadgets::Event::system_call);

	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		for (const bool acquire : {false, true})
			for (const bool release : {false, true})
				run_gadget_variant(mapping, map_base,
					compare_swap(size_code, acquire, release, 2, 3, 4),
					size);
	}
	for (const bool acquire : {false, true})
		for (const bool release : {false, true})
			run_gadget_variant(mapping, map_base,
				unprivileged_compare_swap(true, acquire, release,
					2, 3, 4), 8);

	std::uint64_t zero{};
	std::memcpy(mapping.at(map_base), &zero, sizeof(zero));
	arm64::CpuState cpu{};
	cpu.sp = map_base;
	cpu.registers[4] = replacement_value;
	test::require(mapping.execute(pc,
		compare_swap(3, true, false, 31, 31, 4), cpu) ==
		gadgets::Event::system_call);
	std::uint64_t actual{};
	std::memcpy(&actual, mapping.at(map_base), sizeof(actual));
	test::require(actual == replacement_value && cpu.sp == map_base);

	for (unsigned size_code = 1; size_code < 4; ++size_code) {
		cpu = {};
		cpu.registers[2] = map_base + 1;
		cpu.registers[3] = initial_value;
		cpu.registers[4] = replacement_value;
		const auto before = cpu.registers;
		test::require(mapping.execute(pc,
			compare_swap(size_code, true, true, 2, 3, 4), cpu) ==
			gadgets::Event::memory_fault);
		test::require(cpu.pc == pc && cpu.registers == before);
	}
	for (const auto instruction : {
		compare_swap(3, false, false, 2, 3, 4, 30),
		unprivileged_compare_swap(true, false, false, 2, 3, 4, 30),
		unprivileged_compare_swap(false, false, false, 2, 3, 4)}) {
		cpu = {};
		test::require(mapping.execute(pc, instruction, cpu) ==
			gadgets::Event::illegal_instruction);
	}
}

} // namespace

int main()
{
	exhaustive_interpreter_variants();
	interpreter_register_edges_and_reservations();
	interpreter_fault_alignment_and_reserved();
	exhaustive_gadget_variants_and_edges();
}
