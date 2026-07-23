// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_test_support.hpp"

#include <array>
#include <cstring>

namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x330'0000;
constexpr std::uint64_t base = 0xd0'0080;
constexpr std::array initial_values{
	std::uint64_t{0xf1e2'd3c4'b5a6'9788},
	std::uint64_t{0x8071'6253'4435'2617}};
constexpr std::array stored_values{
	std::uint64_t{0x1021'3243'5465'7687},
	std::uint64_t{0x899a'abbc'cdde'eff0}};

[[nodiscard]] constexpr std::uint32_t pair_exclusive(bool wide, bool load,
	bool acquire_release, unsigned rn, unsigned rt, unsigned rt2,
	unsigned status = 31)
{
	return 0x8820'0000U | (static_cast<unsigned>(wide) << 30) |
		(static_cast<unsigned>(load) << 22) |
		((status & 31U) << 16) |
		(static_cast<unsigned>(acquire_release) << 15) |
		((rt2 & 31U) << 10) | ((rn & 31U) << 5) | (rt & 31U);
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

void run_interpreter_variant(bool wide, bool acquire_release, bool direct)
{
	const auto element_size = wide ? 8U : 4U;
	const auto mask = size_mask(element_size);
	AtomicMemory memory;
	memory.direct = direct;
	memory.put_pair(base, initial_values, element_size);
	arm64::CpuState cpu{};
	test::fill_registers(cpu);
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[5] = stored_values[0];
	cpu.registers[6] = stored_values[1];
	cpu.registers[7] = ~std::uint64_t{0};
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		pair_exclusive(wide, true, acquire_release, 2, 3, 4)).reason ==
		arm64::StopReason::continued);
	test::require(cpu.registers[3] == (initial_values[0] & mask));
	test::require(cpu.registers[4] == (initial_values[1] & mask));
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		pair_exclusive(wide, false, acquire_release, 2, 5, 6, 7)).reason ==
		arm64::StopReason::continued);
	test::require(cpu.registers[7] == 0);
	const auto actual = memory.get_pair(base, element_size);
	test::require(actual[0] == (stored_values[0] & mask));
	test::require(actual[1] == (stored_values[1] & mask));

	/* No reservation: status is one and the pair remains unchanged. */
	memory.put_pair(base, initial_values, element_size);
	cpu = {};
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[5] = stored_values[0];
	cpu.registers[6] = stored_values[1];
	cpu.registers[7] = ~std::uint64_t{0};
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		pair_exclusive(wide, false, acquire_release, 2, 5, 6, 7)).reason ==
		arm64::StopReason::continued);
	test::require(cpu.registers[7] == 1);
	const auto unchanged = memory.get_pair(base, element_size);
	test::require(unchanged[0] == (initial_values[0] & mask));
	test::require(unchanged[1] == (initial_values[1] & mask));
}

void exhaustive_interpreter_variants()
{
	for (const bool direct : {false, true})
		for (const bool wide : {false, true})
			for (const bool acquire_release : {false, true})
				run_interpreter_variant(wide, acquire_release, direct);
}

void interpreter_reservation_size_and_store_invalidation()
{
	for (const bool direct : {false, true}) {
		AtomicMemory memory;
		memory.direct = direct;
		memory.put_pair(base, initial_values, 8);
		arm64::CpuState cpu{};
		cpu.pc = pc;
		cpu.registers[2] = base;
		cpu.registers[5] = stored_values[0];
		cpu.registers[6] = stored_values[1];
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			pair_exclusive(true, true, false, 2, 3, 4)).reason ==
			arm64::StopReason::continued);
		cpu.registers[8] = initial_values[1];
		cpu.registers[9] = base + 8;
		/* An equal-value scalar store to the second element conflicts. */
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			test::unsigned_immediate(3, 0, 0, 9, 8)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			pair_exclusive(true, false, false, 2, 5, 6, 7)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[7] == 1);

		/* Different monitored byte widths never satisfy one another. */
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			pair_exclusive(true, true, false, 2, 3, 4)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			scalar_exclusive(3, false, 2, 5, 7)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[7] == 1);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			scalar_exclusive(3, true, 2, 3)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			pair_exclusive(true, false, false, 2, 5, 6, 7)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[7] == 1);

		/* The monitor records the byte range, not the syntactic shape: a
		 * 32-bit pair and a 64-bit scalar both cover exactly eight bytes. */
		memory.put_pair(base, initial_values, 4);
		cpu = {};
		cpu.pc = pc;
		cpu.registers[2] = base;
		cpu.registers[5] = (stored_values[0] & 0xffff'ffffU) |
			(stored_values[1] << 32);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			pair_exclusive(false, true, false, 2, 3, 4)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			scalar_exclusive(3, false, 2, 5, 7)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[7] == 0);

		memory.put_pair(base, initial_values, 4);
		cpu = {};
		cpu.pc = pc;
		cpu.registers[2] = base;
		cpu.registers[5] = stored_values[0];
		cpu.registers[6] = stored_values[1];
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			scalar_exclusive(3, true, 2, 3)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			pair_exclusive(false, false, false, 2, 5, 6, 7)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[7] == 0);
	}
}

void interpreter_edges_faults_and_constraints()
{
	for (const bool wide : {false, true}) {
		const auto element_size = wide ? 8U : 4U;
		const auto total_size = element_size * 2;
		const auto mask = size_mask(element_size);
		AtomicMemory memory;
		memory.put_pair(base, initial_values, element_size);
		arm64::CpuState cpu{};
		cpu.pc = pc;
		cpu.sp = base;
		/* XZR can independently discard/supply either element and status. */
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			pair_exclusive(wide, true, true, 31, 31, 4)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[4] == (initial_values[1] & mask));
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			pair_exclusive(wide, false, true, 31, 31, 4, 31)).reason ==
			arm64::StopReason::continued);
		const auto zero_first = memory.get_pair(base, element_size);
		test::require(zero_first[0] == 0 &&
			zero_first[1] == (initial_values[1] & mask));
		test::require(cpu.sp == base);

		memory = {};
		cpu = {};
		cpu.pc = pc;
		cpu.registers[2] = base + element_size;
		cpu.registers[5] = stored_values[0];
		cpu.registers[6] = stored_values[1];
		const auto before = cpu;
		for (const bool load : {false, true}) {
			const auto result = arm64::Dispatcher{}.execute(cpu, memory,
				pair_exclusive(wide, load, true, 2, 5, 6,
					load ? 31U : 7U));
			test::require(result.reason == arm64::StopReason::memory_fault);
			test::require(result.address == base + element_size &&
				result.write == !load);
			test::require(cpu.pc == before.pc &&
				cpu.registers == before.registers);
		}
		test::require(total_size == (wide ? 16U : 8U));
	}

	/* The selected LDPOVERLAP constraint is UNDEFINED; a non-31 Rs on a load
	 * is also reserved. Neither case may touch memory. */
	for (const auto instruction : {
		pair_exclusive(false, true, false, 2, 3, 3),
		pair_exclusive(true, true, true, 2, 3, 4, 30),
		pair_exclusive(true, true, true, 2, 3, 3, 30),
		pair_exclusive(false, true, false, 2, 3, 4) & 0x7fff'ffffU}) {
		AtomicMemory memory;
		arm64::CpuState cpu{};
		cpu.pc = pc;
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			instruction).reason == arm64::StopReason::illegal_instruction);
		test::require(cpu.pc == pc && memory.atomic_pointers == 0);
	}

	/* DATAOVERLAP and BASEOVERLAP select UNKNOWN. This implementation's
	 * permitted deterministic choice uses the original address/data. */
	AtomicMemory memory;
	memory.put_pair(base, initial_values, 8);
	arm64::CpuState cpu{};
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[5] = stored_values[0];
	cpu.registers[6] = stored_values[1];
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		pair_exclusive(true, true, false, 2, 3, 4)).reason ==
		arm64::StopReason::continued);
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		pair_exclusive(true, false, false, 2, 5, 6, 5)).reason ==
		arm64::StopReason::continued);
	test::require(cpu.registers[5] == 0);
	const auto data_overlap = memory.get_pair(base, 8);
	test::require(data_overlap[0] == stored_values[0] &&
		data_overlap[1] == stored_values[1]);

	memory.put_pair(base, initial_values, 8);
	cpu = {};
	cpu.pc = pc;
	cpu.registers[2] = base;
	cpu.registers[5] = stored_values[0];
	cpu.registers[6] = stored_values[1];
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		pair_exclusive(true, true, false, 2, 3, 4)).reason ==
		arm64::StopReason::continued);
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		pair_exclusive(true, false, false, 2, 5, 6, 2)).reason ==
		arm64::StopReason::continued);
	test::require(cpu.registers[2] == 0);
	const auto base_overlap = memory.get_pair(base, 8);
	test::require(base_overlap[0] == stored_values[0] &&
		base_overlap[1] == stored_values[1]);

	memory = {};
	memory.put_pair(base, initial_values, 8);
	cpu = {};
	cpu.pc = pc;
	cpu.registers[2] = base;
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		pair_exclusive(true, true, false, 2, 3, 4)).reason ==
		arm64::StopReason::continued);
	memory.fault = true;
	const auto before = cpu;
	const auto fault = arm64::Dispatcher{}.execute(cpu, memory,
		pair_exclusive(true, false, false, 2, 5, 6, 7));
	test::require(fault.reason == arm64::StopReason::memory_fault &&
		fault.address == base && fault.write);
	test::require(cpu.pc == before.pc && cpu.registers == before.registers);
}

void run_gadget_variant(test::StaticMapping &mapping, std::uint64_t map_base,
	bool wide, bool acquire_release)
{
	const auto element_size = wide ? 8U : 4U;
	const auto mask = size_mask(element_size);
	arm64::CpuState cpu{};
	const std::array stream{
		pair_exclusive(wide, true, acquire_release, 2, 3, 4),
		pair_exclusive(wide, false, acquire_release, 2, 5, 6, 7),
		0xd400'0001U};
	bool stored = false;
	for (unsigned attempt = 0; attempt < 16 && !stored; ++attempt) {
		std::memcpy(mapping.at(map_base), &initial_values[0], element_size);
		std::memcpy(mapping.at(map_base + element_size), &initial_values[1],
			element_size);
		cpu = {};
		cpu.registers[2] = map_base;
		cpu.registers[5] = stored_values[0];
		cpu.registers[6] = stored_values[1];
		cpu.registers[7] = ~std::uint64_t{0};
		test::require(mapping.execute(pc,
			std::span<const std::uint32_t>{stream}, cpu) ==
			gadgets::Event::system_call);
		test::require(cpu.registers[3] == (initial_values[0] & mask));
		test::require(cpu.registers[4] == (initial_values[1] & mask));
		stored = cpu.registers[7] == 0;
	}
	test::require(stored);
	std::array<std::uint64_t, 2> actual{};
	std::memcpy(&actual[0], mapping.at(map_base), element_size);
	std::memcpy(&actual[1], mapping.at(map_base + element_size), element_size);
	test::require(actual[0] == (stored_values[0] & mask));
	test::require(actual[1] == (stored_values[1] & mask));

	cpu.registers[7] = ~std::uint64_t{0};
	test::require(mapping.execute(pc,
		pair_exclusive(wide, false, acquire_release, 2, 5, 6, 7), cpu) ==
		gadgets::Event::system_call);
	test::require(cpu.registers[7] == 1);
}

void gadget_variants_and_edges()
{
	test::StaticMapping mapping{0x350'0000, 163,
		"/private/tmp/uml-base-exclusive-pair.XXXXXX"};
	const auto map_base = mapping.guest() +
		4 * test::StaticMapping::page_size;
	arm64::CpuState warm{};
	warm.registers[2] = map_base;
	warm.registers[3] = initial_values[0];
	test::require(mapping.execute(pc,
		test::unsigned_immediate(3, 1, 0, 2, 4), warm) ==
		gadgets::Event::system_call);
	test::require(mapping.execute(pc,
		test::unsigned_immediate(3, 0, 0, 2, 3), warm) ==
		gadgets::Event::system_call);
	for (const bool wide : {false, true})
		for (const bool acquire_release : {false, true})
			run_gadget_variant(mapping, map_base, wide, acquire_release);

	std::memcpy(mapping.at(map_base), initial_values.data(),
		sizeof(initial_values));
	arm64::CpuState cpu{};
	cpu.registers[2] = map_base;
	cpu.registers[5] = stored_values[0];
	cpu.registers[6] = stored_values[1];
	cpu.registers[8] = initial_values[1];
	cpu.registers[9] = map_base + 8;
	const std::array invalidate_stream{
		pair_exclusive(true, true, false, 2, 3, 4),
		test::unsigned_immediate(3, 0, 0, 9, 8),
		pair_exclusive(true, false, false, 2, 5, 6, 7),
		0xd400'0001U};
	test::require(mapping.execute(pc,
		std::span<const std::uint32_t>{invalidate_stream}, cpu) ==
		gadgets::Event::system_call);
	test::require(cpu.registers[7] == 1);

	/* Same eight-byte monitor range across pair-W and scalar-X forms. */
	const std::array pair_to_scalar{
		pair_exclusive(false, true, false, 2, 3, 4),
		scalar_exclusive(3, false, 2, 5, 7), 0xd400'0001U};
	const auto scalar_replacement = (stored_values[0] & 0xffff'ffffU) |
		((stored_values[1] & 0xffff'ffffU) << 32);
	bool pair_to_scalar_succeeded = false;
	for (unsigned attempt = 0; attempt < 16; ++attempt) {
		std::memcpy(mapping.at(map_base), initial_values.data(), 8);
		cpu = {};
		cpu.registers[2] = map_base;
		cpu.registers[5] = scalar_replacement;
		test::require(mapping.execute(pc,
			std::span<const std::uint32_t>{pair_to_scalar}, cpu) ==
			gadgets::Event::system_call);
		if (cpu.registers[7] == 0) {
			pair_to_scalar_succeeded = true;
			break;
		}
		std::uint64_t unchanged{};
		std::memcpy(&unchanged, mapping.at(map_base), 8);
		test::require(unchanged == initial_values[0]);
	}
	test::require(pair_to_scalar_succeeded);
	std::uint64_t scalar_actual{};
	std::memcpy(&scalar_actual, mapping.at(map_base), 8);
	test::require(scalar_actual == scalar_replacement);

	const std::array scalar_to_pair{
		scalar_exclusive(3, true, 2, 3),
		pair_exclusive(false, false, false, 2, 5, 6, 7),
		0xd400'0001U};
	bool scalar_to_pair_succeeded = false;
	for (unsigned attempt = 0; attempt < 16; ++attempt) {
		std::memcpy(mapping.at(map_base), initial_values.data(), 8);
		cpu = {};
		cpu.registers[2] = map_base;
		cpu.registers[5] = stored_values[0];
		cpu.registers[6] = stored_values[1];
		test::require(mapping.execute(pc,
			std::span<const std::uint32_t>{scalar_to_pair}, cpu) ==
			gadgets::Event::system_call);
		if (cpu.registers[7] == 0) {
			scalar_to_pair_succeeded = true;
			break;
		}
		std::uint64_t unchanged{};
		std::memcpy(&unchanged, mapping.at(map_base), 8);
		test::require(unchanged == initial_values[0]);
	}
	test::require(scalar_to_pair_succeeded);
	std::array<std::uint32_t, 2> pair_actual{};
	std::memcpy(pair_actual.data(), mapping.at(map_base), 8);
	test::require(pair_actual[0] == static_cast<std::uint32_t>(stored_values[0]) &&
		pair_actual[1] == static_cast<std::uint32_t>(stored_values[1]));

	cpu = {};
	cpu.sp = map_base;
	const std::array zero_stream{
		pair_exclusive(true, true, true, 31, 31, 4),
		pair_exclusive(true, false, true, 31, 31, 4, 31),
		0xd400'0001U};
	test::require(mapping.execute(pc,
		std::span<const std::uint32_t>{zero_stream}, cpu) ==
		gadgets::Event::system_call);
	test::require(cpu.sp == map_base);

	for (const bool wide : {false, true}) {
		const auto element_size = wide ? 8U : 4U;
		for (const bool load : {false, true}) {
			cpu = {};
			cpu.registers[2] = map_base + element_size;
			cpu.registers[5] = stored_values[0];
			cpu.registers[6] = stored_values[1];
			const auto before = cpu.registers;
			test::require(mapping.execute(pc,
				pair_exclusive(wide, load, true, 2, 5, 6,
					load ? 31U : 7U), cpu) ==
				gadgets::Event::memory_fault);
			test::require(cpu.pc == pc && cpu.registers == before);
		}
	}

	for (const auto instruction : {
		pair_exclusive(false, true, false, 2, 3, 3),
		pair_exclusive(true, true, true, 2, 3, 4, 30),
		pair_exclusive(true, true, true, 2, 3, 3, 30)}) {
		cpu = {};
		test::require(mapping.execute(pc, instruction, cpu) ==
			gadgets::Event::illegal_instruction);
	}
}

} // namespace

int main()
{
	exhaustive_interpreter_variants();
	interpreter_reservation_size_and_store_invalidation();
	interpreter_edges_faults_and_constraints();
	gadget_variants_and_edges();
}
