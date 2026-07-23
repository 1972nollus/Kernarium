// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_test_support.hpp"

#include <atomic>
#include <array>
#include <cstring>
#include <thread>

namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x3b0'0000;
constexpr std::uint64_t base = 0xf0'0080;
constexpr std::array initial_values{
	std::uint64_t{0xf1e2'd3c4'b5a6'9788},
	std::uint64_t{0x8071'6253'4435'2617}};
constexpr std::array replacement_values{
	std::uint64_t{0x1021'3243'5465'7687},
	std::uint64_t{0x899a'abbc'cdde'eff0}};

[[nodiscard]] constexpr std::uint32_t compare_swap_pair(bool wide,
	bool acquire, bool release, unsigned rn, unsigned rs, unsigned rt,
	unsigned rt2 = 31)
{
	return 0x0820'0000U | (static_cast<unsigned>(wide) << 30) |
		(static_cast<unsigned>(acquire) << 22) | ((rs & 31U) << 16) |
		(static_cast<unsigned>(release) << 15) | ((rt2 & 31U) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t unprivileged_compare_swap_pair(
	bool wide, bool acquire, bool release, unsigned rn, unsigned rs,
	unsigned rt, unsigned rt2 = 31)
{
	return 0x0980'0000U | (static_cast<unsigned>(wide) << 30) |
		(static_cast<unsigned>(acquire) << 22) | ((rs & 31U) << 16) |
		(static_cast<unsigned>(release) << 15) | ((rt2 & 31U) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t exclusive_pair(bool wide, bool load,
	unsigned rn, unsigned rt, unsigned rt2, unsigned status = 31)
{
	return 0x8820'0000U | (static_cast<unsigned>(wide) << 30) |
		(static_cast<unsigned>(load) << 22) | ((status & 31U) << 16) |
		((rt2 & 31U) << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint64_t size_mask(std::size_t size)
{
	return size == 8 ? ~std::uint64_t{0} : 0xffff'ffffU;
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

	void put_pair(std::uint64_t address,
		const std::array<std::uint64_t, 2> &values, std::size_t element_size)
	{
		std::memcpy(pointer(address), &values[0], element_size);
		std::memcpy(pointer(address) + element_size, &values[1], element_size);
	}

	[[nodiscard]] std::array<std::uint64_t, 2> get_pair(
		std::uint64_t address, std::size_t element_size) const
	{
		std::array<std::uint64_t, 2> result{};
		std::memcpy(&result[0], pointer(address), element_size);
		std::memcpy(&result[1], pointer(address) + element_size, element_size);
		return result;
	}

	[[nodiscard]] std::uint64_t *host_pair(std::uint64_t address)
	{
		return reinterpret_cast<std::uint64_t *>(pointer(address));
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
		return address >= base - 128 &&
			address - (base - 128) <= bytes.size() &&
			size <= bytes.size() - (address - (base - 128));
	}

	[[nodiscard]] std::byte *pointer(std::uint64_t address)
	{
		return bytes.data() + (address - (base - 128));
	}

	[[nodiscard]] const std::byte *pointer(std::uint64_t address) const
	{
		return bytes.data() + (address - (base - 128));
	}

	alignas(64) std::array<std::byte, 512> bytes{};
};

void native_store_pair(std::uint64_t *pointer,
	const std::array<std::uint64_t, 2> &pair)
{
	std::uint64_t old_first;
	std::uint64_t old_second;
	std::uint32_t status;
	__asm__ volatile(
		"1:\n"
		"ldxp %0, %1, [%3]\n"
		"stlxp %w2, %4, %5, [%3]\n"
		"cbnz %w2, 1b"
		: "=&r"(old_first), "=&r"(old_second), "=&r"(status)
		: "r"(pointer), "r"(pair[0]), "r"(pair[1])
		: "memory");
	(void)old_first;
	(void)old_second;
}

void run_interpreter_variant(std::uint32_t instruction,
	std::size_t element_size, bool direct)
{
	const auto mask = size_mask(element_size);
	AtomicMemory memory;
	memory.direct = direct;
	memory.put_pair(base, initial_values, element_size);
	arm64::CpuState cpu{};
	test::fill_registers(cpu);
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[4] = initial_values[0];
	cpu.registers[5] = initial_values[1];
	cpu.registers[6] = replacement_values[0];
	cpu.registers[7] = replacement_values[1];
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		instruction).reason == arm64::StopReason::continued);
	test::require(cpu.registers[4] == (initial_values[0] & mask));
	test::require(cpu.registers[5] == (initial_values[1] & mask));
	const auto replaced = memory.get_pair(base, element_size);
	test::require(replaced[0] == (replacement_values[0] & mask));
	test::require(replaced[1] == (replacement_values[1] & mask));

	memory.put_pair(base, initial_values, element_size);
	cpu.registers[4] = initial_values[0] ^ 1U;
	cpu.registers[5] = initial_values[1];
	cpu.registers[6] = replacement_values[0];
	cpu.registers[7] = replacement_values[1];
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		instruction).reason == arm64::StopReason::continued);
	test::require(cpu.registers[4] == (initial_values[0] & mask));
	test::require(cpu.registers[5] == (initial_values[1] & mask));
	const auto unchanged = memory.get_pair(base, element_size);
	test::require(unchanged[0] == (initial_values[0] & mask));
	test::require(unchanged[1] == (initial_values[1] & mask));
}

void exhaustive_interpreter_variants()
{
	for (const bool direct : {false, true}) {
		for (const bool wide : {false, true})
			for (const bool acquire : {false, true})
				for (const bool release : {false, true})
					run_interpreter_variant(compare_swap_pair(wide,
						acquire, release, 2, 4, 6), wide ? 8U : 4U,
						direct);
		for (const bool acquire : {false, true})
			for (const bool release : {false, true})
				run_interpreter_variant(unprivileged_compare_swap_pair(true,
					acquire, release, 2, 4, 6), 8, direct);
	}
}

void interpreter_single_copy_atomicity()
{
	AtomicMemory memory;
	memory.put_pair(base, initial_values, 8);
	auto *host = memory.host_pair(base);
	std::atomic<bool> running{true};
	std::atomic<bool> ready{false};
	std::thread writer{[&] {
		ready.store(true, std::memory_order_release);
		while (running.load(std::memory_order_acquire)) {
			native_store_pair(host, replacement_values);
			native_store_pair(host, initial_values);
		}
	}};
	while (!ready.load(std::memory_order_acquire))
		std::this_thread::yield();
	for (unsigned iteration = 0; iteration < 5000; ++iteration) {
		arm64::CpuState cpu{};
		cpu.pc = pc;
		cpu.registers[2] = base;
		/* An impossible comparison turns CASP into an atomic observing load. */
		cpu.registers[4] = 0;
		cpu.registers[5] = 0;
		cpu.registers[6] = ~std::uint64_t{0};
		cpu.registers[7] = ~std::uint64_t{0};
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			compare_swap_pair(true, true, true, 2, 4, 6)).reason ==
			arm64::StopReason::continued);
		const std::array observed{cpu.registers[4], cpu.registers[5]};
		test::require(observed == initial_values ||
			observed == replacement_values);
	}
	running.store(false, std::memory_order_release);
	writer.join();
}

void interpreter_edges_reservations_and_faults()
{
	AtomicMemory memory;
	std::array<std::uint64_t, 2> with_zero{initial_values[0], 0};
	memory.put_pair(base, with_zero, 8);
	arm64::CpuState cpu{};
	cpu.pc = pc;
	cpu.sp = base;
	cpu.registers[30] = initial_values[0];
	cpu.registers[4] = replacement_values[0];
	cpu.registers[5] = replacement_values[1];
	/* Pair register 30 includes XZR as its second element. */
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		compare_swap_pair(true, true, true, 31, 30, 4)).reason ==
		arm64::StopReason::continued);
	test::require(cpu.registers[30] == initial_values[0] && cpu.sp == base);
	const auto zr_compare = memory.get_pair(base, 8);
	test::require(zr_compare[0] == replacement_values[0] &&
		zr_compare[1] == replacement_values[1]);

	/* A replacement pair starting at X30 supplies zero for its second value. */
	memory.put_pair(base, initial_values, 8);
	cpu = {};
	cpu.pc = pc;
	cpu.sp = base;
	cpu.registers[4] = initial_values[0];
	cpu.registers[5] = initial_values[1];
	cpu.registers[30] = replacement_values[0];
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		compare_swap_pair(true, false, false, 31, 4, 30)).reason ==
		arm64::StopReason::continued);
	const auto zr_replacement = memory.get_pair(base, 8);
	test::require(zr_replacement[0] == replacement_values[0] &&
		zr_replacement[1] == 0);

	/* Input pairs are captured before an overlapping result pair is written. */
	memory.put_pair(base, initial_values, 8);
	cpu.registers[4] = initial_values[0];
	cpu.registers[5] = initial_values[1];
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		compare_swap_pair(true, false, false, 31, 4, 4)).reason ==
		arm64::StopReason::continued);
	const auto same_pair = memory.get_pair(base, 8);
	test::require(same_pair == initial_values);

	/* Failed pair CAS invalidates a pair-exclusive monitor. */
	memory.put_pair(base, initial_values, 8);
	cpu = {};
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[4] = replacement_values[0];
	cpu.registers[5] = replacement_values[1];
	cpu.registers[6] = replacement_values[0];
	cpu.registers[7] = replacement_values[1];
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		exclusive_pair(true, true, 2, 8, 9)).reason ==
		arm64::StopReason::continued);
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		compare_swap_pair(true, false, false, 2, 4, 6)).reason ==
		arm64::StopReason::continued);
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		exclusive_pair(true, false, 2, 6, 7, 10)).reason ==
		arm64::StopReason::continued);
	test::require(cpu.registers[10] == 1);

	for (const bool wide : {false, true}) {
		const auto element_size = wide ? 8U : 4U;
		memory = {};
		cpu = {};
		cpu.pc = pc;
		cpu.registers[2] = base + element_size;
		cpu.registers[4] = initial_values[0];
		cpu.registers[5] = initial_values[1];
		cpu.registers[6] = replacement_values[0];
		cpu.registers[7] = replacement_values[1];
		const auto before = cpu;
		const auto result = arm64::Dispatcher{}.execute(cpu, memory,
			compare_swap_pair(wide, true, true, 2, 4, 6));
		test::require(result.reason == arm64::StopReason::memory_fault &&
			result.address == base + element_size && result.write);
		test::require(cpu.pc == pc && cpu.registers == before.registers &&
			memory.atomic_pointers == 0);
	}

	memory = {};
	memory.fault = true;
	cpu = {};
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[4] = initial_values[0];
	cpu.registers[5] = initial_values[1];
	cpu.registers[6] = replacement_values[0];
	cpu.registers[7] = replacement_values[1];
	const auto before = cpu;
	const auto fault = arm64::Dispatcher{}.execute(cpu, memory,
		compare_swap_pair(true, true, true, 2, 4, 6));
	test::require(fault.reason == arm64::StopReason::memory_fault &&
		fault.address == base && fault.write);
	test::require(cpu.pc == pc && cpu.registers == before.registers);

	for (const auto instruction : {
		compare_swap_pair(true, false, false, 2, 5, 6),
		compare_swap_pair(true, false, false, 2, 4, 7),
		compare_swap_pair(true, false, false, 2, 4, 6, 30),
		unprivileged_compare_swap_pair(false, false, false, 2, 4, 6),
		unprivileged_compare_swap_pair(true, false, false, 2, 5, 6),
		unprivileged_compare_swap_pair(true, false, false, 2, 4, 7)}) {
		memory = {};
		cpu = {};
		cpu.pc = pc;
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			instruction).reason == arm64::StopReason::illegal_instruction);
		test::require(cpu.pc == pc && memory.atomic_pointers == 0);
	}
}

void run_gadget_variant(test::StaticMapping &mapping, std::uint64_t map_base,
	std::uint32_t instruction, std::size_t element_size)
{
	const auto mask = size_mask(element_size);
	std::memcpy(mapping.at(map_base), &initial_values[0], element_size);
	std::memcpy(mapping.at(map_base + element_size), &initial_values[1],
		element_size);
	arm64::CpuState cpu{};
	cpu.registers[2] = map_base;
	cpu.registers[4] = initial_values[0];
	cpu.registers[5] = initial_values[1];
	cpu.registers[6] = replacement_values[0];
	cpu.registers[7] = replacement_values[1];
	test::require(mapping.execute(pc, instruction, cpu) ==
		gadgets::Event::system_call);
	test::require(cpu.registers[4] == (initial_values[0] & mask));
	test::require(cpu.registers[5] == (initial_values[1] & mask));
	std::array<std::uint64_t, 2> actual{};
	std::memcpy(&actual[0], mapping.at(map_base), element_size);
	std::memcpy(&actual[1], mapping.at(map_base + element_size), element_size);
	test::require(actual[0] == (replacement_values[0] & mask));
	test::require(actual[1] == (replacement_values[1] & mask));

	std::memcpy(mapping.at(map_base), &initial_values[0], element_size);
	std::memcpy(mapping.at(map_base + element_size), &initial_values[1],
		element_size);
	cpu.registers[4] = initial_values[0] ^ 1U;
	cpu.registers[5] = initial_values[1];
	test::require(mapping.execute(pc, instruction, cpu) ==
		gadgets::Event::system_call);
	test::require(cpu.registers[4] == (initial_values[0] & mask));
	test::require(cpu.registers[5] == (initial_values[1] & mask));
}

void exhaustive_gadget_variants_and_reserved()
{
	test::StaticMapping mapping{0x3d0'0000, 181,
		"/private/tmp/uml-base-compare-swap-pair.XXXXXX"};
	const auto map_base = mapping.guest() +
		4 * test::StaticMapping::page_size;
	arm64::CpuState warm{};
	warm.registers[2] = map_base;
	warm.registers[3] = initial_values[0];
	test::require(mapping.execute(pc,
		test::unsigned_immediate(3, 0, 0, 2, 3), warm) ==
		gadgets::Event::system_call);

	for (const bool wide : {false, true})
		for (const bool acquire : {false, true})
			for (const bool release : {false, true})
				run_gadget_variant(mapping, map_base,
					compare_swap_pair(wide, acquire, release, 2, 4, 6),
					wide ? 8U : 4U);
	for (const bool acquire : {false, true})
		for (const bool release : {false, true})
			run_gadget_variant(mapping, map_base,
				unprivileged_compare_swap_pair(true, acquire, release,
					2, 4, 6), 8);

	arm64::CpuState cpu{};
	for (const bool wide : {false, true}) {
		const auto element_size = wide ? 8U : 4U;
		cpu = {};
		cpu.registers[2] = map_base + element_size;
		cpu.registers[4] = initial_values[0];
		cpu.registers[5] = initial_values[1];
		cpu.registers[6] = replacement_values[0];
		cpu.registers[7] = replacement_values[1];
		const auto before = cpu.registers;
		test::require(mapping.execute(pc,
			compare_swap_pair(wide, true, true, 2, 4, 6), cpu) ==
			gadgets::Event::memory_fault);
		test::require(cpu.pc == pc && cpu.registers == before);
	}
	for (const auto instruction : {
		compare_swap_pair(true, false, false, 2, 5, 6),
		compare_swap_pair(true, false, false, 2, 4, 7),
		compare_swap_pair(true, false, false, 2, 4, 6, 30),
		unprivileged_compare_swap_pair(false, false, false, 2, 4, 6),
		unprivileged_compare_swap_pair(true, false, false, 2, 5, 6),
		unprivileged_compare_swap_pair(true, false, false, 2, 4, 7)}) {
		cpu = {};
		test::require(mapping.execute(pc, instruction, cpu) ==
			gadgets::Event::illegal_instruction);
	}
}

} // namespace

int main()
{
	exhaustive_interpreter_variants();
	interpreter_single_copy_atomicity();
	interpreter_edges_reservations_and_faults();
	exhaustive_gadget_variants_and_reserved();
}
