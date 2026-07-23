// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_test_support.hpp"

#include <array>
#include <cstring>

namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x2d0'0000;
constexpr std::uint64_t base = 0xc0'0040;
constexpr std::uint64_t initial_value = 0xf1e2'd3c4'b5a6'9788ULL;
constexpr std::uint64_t stored_value = 0x1021'3243'5465'7687ULL;

[[nodiscard]] constexpr std::uint32_t exclusive(unsigned size_code, bool load,
	bool acquire_release, unsigned rn, unsigned rt, unsigned status = 31,
	unsigned rt2 = 31)
{
	return 0x0800'0000U | ((size_code & 3U) << 30) |
		(static_cast<unsigned>(load) << 22) |
		((status & 31U) << 16) |
		(static_cast<unsigned>(acquire_release) << 15) |
		((rt2 & 31U) << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t unprivileged_exclusive(bool wide,
	bool load, bool acquire_release, unsigned rn, unsigned rt,
	unsigned status = 31, unsigned rt2 = 31)
{
	return 0x8900'0000U | (static_cast<unsigned>(wide) << 30) |
		(static_cast<unsigned>(load) << 22) |
		((status & 31U) << 16) |
		(static_cast<unsigned>(acquire_release) << 15) |
		((rt2 & 31U) << 10) | ((rn & 31U) << 5) | (rt & 31U);
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

void run_interpreter_variant(std::uint32_t load_instruction,
	std::uint32_t store_instruction, std::size_t size, bool direct)
{
	AtomicMemory memory;
	memory.direct = direct;
	memory.put(base, initial_value, size);
	arm64::CpuState cpu{};
	test::fill_registers(cpu);
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[4] = stored_value;
	cpu.registers[5] = ~std::uint64_t{0};
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		load_instruction).reason == arm64::StopReason::continued);
	test::require(cpu.registers[3] == (initial_value & size_mask(size)));
	test::require(cpu.pc == pc + 4);
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		store_instruction).reason == arm64::StopReason::continued);
	test::require(cpu.registers[5] == 0);
	test::require(memory.get(base, size) == (stored_value & size_mask(size)));
	test::require(cpu.pc == pc + 8);

	/* A Store-Exclusive with no matching monitor must report failure, perform
	 * no write, and zero-extend its W status result into the X register. */
	memory.put(base, initial_value, size);
	cpu = {};
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[4] = stored_value;
	cpu.registers[5] = ~std::uint64_t{0};
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		store_instruction).reason == arm64::StopReason::continued);
	test::require(cpu.registers[5] == 1);
	test::require(memory.get(base, size) == (initial_value & size_mask(size)));
}

void exhaustive_interpreter_variants()
{
	for (const bool direct : {false, true}) {
		for (unsigned size_code = 0; size_code < 4; ++size_code) {
			const auto size = std::size_t{1} << size_code;
			for (const bool acquire_release : {false, true})
				run_interpreter_variant(
					exclusive(size_code, true, acquire_release, 2, 3),
					exclusive(size_code, false, acquire_release, 2, 4, 5),
					size, direct);
		}
		for (const bool wide : {false, true}) {
			const auto size = wide ? 8U : 4U;
			for (const bool acquire_release : {false, true})
				run_interpreter_variant(
					unprivileged_exclusive(wide, true,
						acquire_release, 2, 3),
					unprivileged_exclusive(wide, false,
						acquire_release, 2, 4, 5),
					size, direct);
		}
	}
}

void interpreter_reservation_invalidation()
{
	for (const bool direct : {false, true}) {
		AtomicMemory memory;
		memory.direct = direct;
		memory.put(base, initial_value, 8);
		arm64::CpuState cpu{};
		cpu.pc = pc;
		cpu.registers[2] = base;
		cpu.registers[3] = stored_value;
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			exclusive(3, true, false, 2, 4)).reason ==
			arm64::StopReason::continued);

		/* Rewriting identical bits is still a conflicting store. */
		cpu.registers[6] = initial_value;
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			test::unsigned_immediate(3, 0, 0, 2, 6)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			exclusive(3, false, false, 2, 3, 5)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[5] == 1);
		test::require(memory.get(base, 8) == initial_value);

		/* A successful Store-Exclusive consumes its monitor. */
		cpu.registers[5] = ~std::uint64_t{0};
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			exclusive(3, true, false, 2, 4)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			exclusive(3, false, false, 2, 3, 5)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[5] == 0);
		cpu.registers[5] = ~std::uint64_t{0};
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			exclusive(3, false, false, 2, 3, 5)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[5] == 1);
	}
}

void interpreter_sp_zero_fault_alignment_and_reserved()
{
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		AtomicMemory memory;
		memory.put(base, initial_value, size);
		arm64::CpuState cpu{};
		cpu.pc = pc;
		cpu.sp = base;
		/* Rt==31 discards the load but still establishes the monitor. */
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			exclusive(size_code, true, true, 31, 31)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.sp == base);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			exclusive(size_code, false, true, 31, 31, 31)).reason ==
			arm64::StopReason::continued);
		test::require(memory.get(base, size) == 0);

		if (size > 1) {
			memory = {};
			cpu = {};
			cpu.pc = pc;
			cpu.registers[2] = base + 1;
			cpu.registers[3] = stored_value;
			const auto before = cpu;
			for (const bool load : {false, true}) {
				const auto result = arm64::Dispatcher{}.execute(cpu, memory,
					exclusive(size_code, load, true, 2, 3,
						load ? 31U : 5U));
				test::require(result.reason == arm64::StopReason::memory_fault);
				test::require(result.address == base + 1 &&
					result.write == !load);
				test::require(cpu.pc == before.pc &&
					cpu.registers == before.registers);
			}
		}
	}

	for (const auto instruction : {
		exclusive(3, true, false, 2, 3, 30, 31),
		exclusive(3, true, false, 2, 3, 31, 30),
		exclusive(3, false, false, 2, 3, 5, 30),
		unprivileged_exclusive(true, true, false, 2, 3, 30, 31),
		unprivileged_exclusive(true, true, false, 2, 3, 31, 30),
		unprivileged_exclusive(true, false, false, 2, 3, 5, 30)}) {
		AtomicMemory memory;
		arm64::CpuState cpu{};
		cpu.pc = pc;
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			instruction).reason == arm64::StopReason::illegal_instruction);
		test::require(cpu.pc == pc && memory.atomic_pointers == 0);
	}

	AtomicMemory memory;
	memory.fault = true;
	arm64::CpuState cpu{};
	cpu.pc = pc;
	cpu.registers[2] = base;
	const auto before = cpu;
	const auto fault = arm64::Dispatcher{}.execute(cpu, memory,
		exclusive(3, true, true, 2, 3));
	test::require(fault.reason == arm64::StopReason::memory_fault);
	test::require(fault.address == base && !fault.write);
	test::require(cpu.pc == pc && cpu.registers == before.registers);
}

void run_gadget_variant(test::StaticMapping &mapping, std::uint64_t map_base,
	std::uint32_t load_instruction, std::uint32_t store_instruction,
	std::size_t size)
{
	arm64::CpuState cpu{};
	const std::array stream{
		load_instruction, store_instruction, 0xd400'0001U};
	bool stored = false;
	for (unsigned attempt = 0; attempt < 16 && !stored; ++attempt) {
		std::memcpy(mapping.at(map_base), &initial_value, size);
		cpu = {};
		cpu.registers[2] = map_base;
		cpu.registers[4] = stored_value;
		cpu.registers[5] = ~std::uint64_t{0};
		test::require(mapping.execute(pc,
			std::span<const std::uint32_t>{stream}, cpu) ==
			gadgets::Event::system_call);
		test::require(cpu.registers[3] ==
			(initial_value & size_mask(size)));
		stored = cpu.registers[5] == 0;
		if (!stored) {
			std::uint64_t unchanged{};
			std::memcpy(&unchanged, mapping.at(map_base), size);
			test::require(unchanged ==
				(initial_value & size_mask(size)));
		}
	}
	/* Store-Exclusive may fail spuriously, notably when its first write-side
	 * TLB lookup calls the miss helper and clears the host monitor. */
	test::require(stored);
	std::uint64_t actual{};
	std::memcpy(&actual, mapping.at(map_base), size);
	test::require(actual == (stored_value & size_mask(size)));

	/* The successful store consumed the local monitor, so a second store in
	 * a fresh stream fails and leaves memory unchanged. */
	cpu.registers[4] = initial_value;
	cpu.registers[5] = ~std::uint64_t{0};
	test::require(mapping.execute(pc, store_instruction, cpu) ==
		gadgets::Event::system_call);
	test::require(cpu.registers[5] == 1);
	std::memcpy(&actual, mapping.at(map_base), size);
	test::require(actual == (stored_value & size_mask(size)));
}

void exhaustive_gadget_variants()
{
	test::StaticMapping mapping{0x2f0'0000, 139,
		"/private/tmp/uml-base-exclusive.XXXXXX"};
	const auto map_base = mapping.guest() +
		4 * test::StaticMapping::page_size;
	arm64::CpuState warm{};
	warm.registers[2] = map_base;
	warm.registers[3] = initial_value;
	/* Populate both read and write TLB entries before relying on the native
	 * exclusive monitor across a threaded instruction sequence. */
	test::require(mapping.execute(pc,
		test::unsigned_immediate(3, 1, 0, 2, 4), warm) ==
		gadgets::Event::system_call);
	test::require(mapping.execute(pc,
		test::unsigned_immediate(3, 0, 0, 2, 3), warm) ==
		gadgets::Event::system_call);

	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		for (const bool acquire_release : {false, true})
			run_gadget_variant(mapping, map_base,
				exclusive(size_code, true, acquire_release, 2, 3),
				exclusive(size_code, false, acquire_release, 2, 4, 5),
				size);
	}
	for (const bool wide : {false, true}) {
		const auto size = wide ? 8U : 4U;
		for (const bool acquire_release : {false, true})
			run_gadget_variant(mapping, map_base,
				unprivileged_exclusive(wide, true,
					acquire_release, 2, 3),
				unprivileged_exclusive(wide, false,
					acquire_release, 2, 4, 5),
				size);
	}
}

void gadget_invalidation_sp_alignment_and_reserved()
{
	test::StaticMapping mapping{0x310'0000, 151,
		"/private/tmp/uml-base-exclusive-edge.XXXXXX"};
	const auto map_base = mapping.guest() +
		4 * test::StaticMapping::page_size;
	std::memcpy(mapping.at(map_base), &initial_value, sizeof(initial_value));

	arm64::CpuState cpu{};
	cpu.registers[2] = map_base;
	cpu.registers[3] = initial_value;
	cpu.registers[4] = stored_value;
	const std::array invalidate_stream{
		exclusive(3, true, false, 2, 6),
		test::unsigned_immediate(3, 0, 0, 2, 3),
		exclusive(3, false, false, 2, 4, 5),
		0xd400'0001U};
	test::require(mapping.execute(pc,
		std::span<const std::uint32_t>{invalidate_stream}, cpu) ==
		gadgets::Event::system_call);
	test::require(cpu.registers[5] == 1);

	const std::array zero_stream{
		exclusive(3, true, true, 31, 31),
		exclusive(3, false, true, 31, 31, 31),
		0xd400'0001U};
	std::uint64_t actual = initial_value;
	for (unsigned attempt = 0; attempt < 16 && actual != 0; ++attempt) {
		std::memcpy(mapping.at(map_base), &initial_value,
			sizeof(initial_value));
		cpu = {};
		cpu.sp = map_base;
		test::require(mapping.execute(pc,
			std::span<const std::uint32_t>{zero_stream}, cpu) ==
			gadgets::Event::system_call);
		std::memcpy(&actual, mapping.at(map_base), sizeof(actual));
		test::require(cpu.sp == map_base);
	}
	/* Native Store-Exclusive is permitted to fail spuriously. Rt/Rs == ZR
	 * discards both the load and status, so observe memory and retry just as
	 * the status-visible variant test above does. */
	test::require(actual == 0);

	for (unsigned size_code = 1; size_code < 4; ++size_code)
		for (const bool load : {false, true}) {
			cpu = {};
			cpu.registers[2] = map_base + 1;
			cpu.registers[3] = stored_value;
			const auto before = cpu.registers;
			test::require(mapping.execute(pc,
				exclusive(size_code, load, true, 2, 3,
					load ? 31U : 5U), cpu) ==
				gadgets::Event::memory_fault);
			test::require(cpu.pc == pc && cpu.registers == before);
		}

	for (const auto instruction : {
		exclusive(3, true, false, 2, 3, 30, 31),
		exclusive(3, true, false, 2, 3, 31, 30),
		exclusive(3, false, false, 2, 3, 5, 30),
		unprivileged_exclusive(true, true, false, 2, 3, 30, 31),
		unprivileged_exclusive(true, true, false, 2, 3, 31, 30),
		unprivileged_exclusive(true, false, false, 2, 3, 5, 30)}) {
		cpu = {};
		test::require(mapping.execute(pc, instruction, cpu) ==
			gadgets::Event::illegal_instruction);
	}
}

} // namespace

int main()
{
	exhaustive_interpreter_variants();
	interpreter_reservation_invalidation();
	interpreter_sp_zero_fault_alignment_and_reserved();
	exhaustive_gadget_variants();
	gadget_invalidation_sp_alignment_and_reserved();
}
