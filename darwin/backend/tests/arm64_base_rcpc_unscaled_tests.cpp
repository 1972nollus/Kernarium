// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_rcpc_test_support.hpp"

#include <array>
#include <cstring>

namespace rcpc = arm64_rcpc_test;
namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

struct Variant final {
	unsigned size_code;
	unsigned operation;
};

constexpr std::array variants{
	Variant{0, 0}, Variant{1, 0}, Variant{2, 0}, Variant{3, 0},
	Variant{0, 1}, Variant{1, 1}, Variant{2, 1}, Variant{3, 1},
	Variant{0, 2}, Variant{1, 2}, Variant{2, 2},
	Variant{0, 3}, Variant{1, 3},
};
constexpr std::uint64_t raw = 0x8172'6354'c536'a788ULL;
constexpr std::uint64_t source = 0x1021'3243'5465'7687ULL;

[[nodiscard]] constexpr std::uint32_t scalar_exclusive(unsigned size_code,
	bool load, unsigned rn, unsigned rt, unsigned status = 31)
{
	return 0x0800'0000U | ((size_code & 3U) << 30) |
		(static_cast<unsigned>(load) << 22) | ((status & 31U) << 16) |
		(31U << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

void exhaustive_interpreter_offsets()
{
	for (const bool direct : {false, true}) {
		for (const auto variant : variants) {
			const auto size = std::size_t{1} << variant.size_code;
			const bool load = variant.operation != 0U;
			for (int offset = -256; offset <= 255; ++offset) {
				rcpc::Memory memory;
				memory.direct = direct;
				const auto address = rcpc::base +
					static_cast<std::uint64_t>(offset);
				if ((address & (size - 1U)) == 0)
					memory.put(address, raw, size);
				arm64::CpuState cpu{};
				cpu.pc = rcpc::pc;
				cpu.registers[2] = rcpc::base;
				cpu.registers[3] = source;
				const auto before = cpu;
				const auto result = arm64::Dispatcher{}.execute(cpu, memory,
					rcpc::unscaled(variant.size_code, variant.operation,
						offset, 2, 3));
				if ((address & (size - 1U)) != 0) {
					test::require(result.reason ==
						arm64::StopReason::memory_fault &&
						result.address == address && result.write == !load);
					test::require(cpu.pc == before.pc &&
						cpu.registers == before.registers &&
						memory.atomic_pointers == 0 && memory.reads == 0 &&
						memory.writes == 0);
					continue;
				}
				test::require(result.reason == arm64::StopReason::continued &&
					cpu.pc == rcpc::pc + 4);
				if (load)
					test::require(cpu.registers[3] == rcpc::expected_load(
						variant.size_code, variant.operation, raw));
				else
					test::require(memory.get(address, size) ==
						(source & rcpc::mask(size)));
				test::require(memory.atomic_pointers == 1);
				if (direct)
					test::require(memory.reads == 0 && memory.writes == 0);
				else if (load)
					test::require(memory.reads == 1 && memory.writes == 0);
				else
					test::require(memory.reads == 0 && memory.writes == 1);
			}
		}
	}
}

void interpreter_register_modes_constraints_and_faults()
{
	for (const auto variant : variants) {
		const auto size = std::size_t{1} << variant.size_code;
		const bool load = variant.operation != 0U;
		rcpc::Memory memory;
		memory.put(rcpc::base, raw, size);
		arm64::CpuState cpu{};
		test::fill_registers(cpu);
		cpu.pc = rcpc::pc;
		cpu.sp = rcpc::base;
		const auto registers = cpu.registers;
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			rcpc::unscaled(variant.size_code, variant.operation, 0, 31,
				31)).reason == arm64::StopReason::continued);
		test::require(cpu.sp == rcpc::base && cpu.registers == registers);
		if (load)
			test::require(memory.get(rcpc::base, size) ==
				(raw & rcpc::mask(size)));
		else
			test::require(memory.get(rcpc::base, size) == 0);

		memory = {};
		memory.put(rcpc::base, raw, size);
		cpu = {};
		cpu.pc = rcpc::pc;
		cpu.registers[2] = rcpc::base;
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			rcpc::unscaled(variant.size_code, variant.operation, 0, 2,
				2)).reason == arm64::StopReason::continued);
		if (load)
			test::require(cpu.registers[2] == rcpc::expected_load(
				variant.size_code, variant.operation, raw));
		else
			test::require(memory.get(rcpc::base, size) ==
				(rcpc::base & rcpc::mask(size)));
	}

	for (const auto invalid : {
		rcpc::unscaled(3, 2, 0, 2, 3),
		rcpc::unscaled(2, 3, 0, 2, 3),
		rcpc::unscaled(3, 3, 0, 2, 3)}) {
		rcpc::Memory memory;
		arm64::CpuState cpu{};
		cpu.pc = rcpc::pc;
		test::require(arm64::Dispatcher{}.execute(cpu, memory, invalid).reason ==
			arm64::StopReason::illegal_instruction);
		test::require(cpu.pc == rcpc::pc && memory.atomic_pointers == 0 &&
			memory.reads == 0 && memory.writes == 0);
	}

	for (const bool load : {false, true}) {
		rcpc::Memory memory;
		memory.fault = true;
		arm64::CpuState cpu{};
		cpu.pc = rcpc::pc;
		cpu.registers[2] = rcpc::base;
		cpu.registers[3] = source;
		const auto before = cpu;
		const auto result = arm64::Dispatcher{}.execute(cpu, memory,
			rcpc::unscaled(3, load ? 1U : 0U, 0, 2, 3));
		test::require(result.reason == arm64::StopReason::memory_fault &&
			result.address == rcpc::base && result.write == !load);
		test::require(cpu.pc == before.pc &&
			cpu.registers == before.registers);
	}
}

void interpreter_store_invalidates_reservation()
{
	for (const bool direct : {false, true}) {
		rcpc::Memory memory;
		memory.direct = direct;
		memory.put(rcpc::base, raw, 8);
		arm64::CpuState cpu{};
		cpu.pc = rcpc::pc;
		cpu.registers[2] = rcpc::base;
		cpu.registers[3] = raw;
		cpu.registers[4] = source;
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			scalar_exclusive(3, true, 2, 5)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			rcpc::unscaled(3, 0, 0, 2, 3)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			scalar_exclusive(3, false, 2, 4, 6)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[6] == 1 && memory.get(rcpc::base, 8) == raw);
	}
}

void exhaustive_gadget_offsets_and_edges()
{
	test::StaticMapping mapping{0x470'0000, 211,
		"/private/tmp/uml-base-rcpc-unscaled.XXXXXX"};
	const auto base = mapping.guest() + 4 * test::StaticMapping::page_size + 512;
	for (const auto variant : variants) {
		const auto size = std::size_t{1} << variant.size_code;
		const bool load = variant.operation != 0U;
		for (int offset = -256; offset <= 255; ++offset) {
			const auto address = base + static_cast<std::uint64_t>(offset);
			if ((address & (size - 1U)) == 0)
				std::memcpy(mapping.at(address), &raw, size);
			arm64::CpuState cpu{};
			cpu.registers[2] = base;
			cpu.registers[3] = source;
			const auto before = cpu.registers;
			const auto event = mapping.execute(rcpc::pc,
				rcpc::unscaled(variant.size_code, variant.operation,
					offset, 2, 3), cpu);
			if ((address & (size - 1U)) != 0) {
				test::require(event == gadgets::Event::memory_fault &&
					cpu.pc == rcpc::pc && cpu.registers == before);
				continue;
			}
			test::require(event == gadgets::Event::system_call);
			if (load)
				test::require(cpu.registers[3] == rcpc::expected_load(
					variant.size_code, variant.operation, raw));
			else {
				std::uint64_t actual{};
				std::memcpy(&actual, mapping.at(address), size);
				test::require(actual == (source & rcpc::mask(size)));
			}
		}

		std::memcpy(mapping.at(base), &raw, size);
		arm64::CpuState cpu{};
		test::fill_registers(cpu);
		cpu.sp = base;
		const auto registers = cpu.registers;
		test::require(mapping.execute(rcpc::pc,
			rcpc::unscaled(variant.size_code, variant.operation, 0, 31,
				31), cpu) == gadgets::Event::system_call);
		test::require(cpu.sp == base && cpu.registers == registers);

		std::memcpy(mapping.at(base), &raw, size);
		cpu = {};
		cpu.registers[2] = base;
		test::require(mapping.execute(rcpc::pc,
			rcpc::unscaled(variant.size_code, variant.operation, 0, 2,
				2), cpu) == gadgets::Event::system_call);
		if (load)
			test::require(cpu.registers[2] == rcpc::expected_load(
				variant.size_code, variant.operation, raw));
		else {
			std::uint64_t actual{};
			std::memcpy(&actual, mapping.at(base), size);
			test::require(actual == (base & rcpc::mask(size)));
		}
	}

	for (const auto invalid : {
		rcpc::unscaled(3, 2, 0, 2, 3),
		rcpc::unscaled(2, 3, 0, 2, 3),
		rcpc::unscaled(3, 3, 0, 2, 3)}) {
		arm64::CpuState cpu{};
		test::require(mapping.execute(rcpc::pc, invalid, cpu) ==
			gadgets::Event::illegal_instruction);
	}

	for (const bool load : {false, true}) {
		arm64::CpuState cpu{};
		cpu.registers[2] = mapping.guest() + test::StaticMapping::length;
		cpu.registers[3] = source;
		const auto before = cpu.registers;
		test::require(mapping.execute(rcpc::pc,
			rcpc::unscaled(3, load ? 1U : 0U, 0, 2, 3), cpu) ==
			gadgets::Event::memory_fault);
		test::require(cpu.pc == rcpc::pc && cpu.registers == before);
	}

	/* Warm both translations before the monitor sequence. */
	arm64::CpuState warm{};
	warm.registers[2] = base;
	warm.registers[3] = raw;
	test::require(mapping.execute(rcpc::pc,
		test::unsigned_immediate(3, 1, 0, 2, 8), warm) ==
		gadgets::Event::system_call);
	test::require(mapping.execute(rcpc::pc,
		test::unsigned_immediate(3, 0, 0, 2, 3), warm) ==
		gadgets::Event::system_call);
	std::memcpy(mapping.at(base), &raw, 8);
	arm64::CpuState cpu{};
	cpu.registers[2] = base;
	cpu.registers[3] = raw;
	cpu.registers[4] = source;
	const std::array stream{
		scalar_exclusive(3, true, 2, 5),
		rcpc::unscaled(3, 0, 0, 2, 3),
		scalar_exclusive(3, false, 2, 4, 6),
		0xd400'0001U};
	test::require(mapping.execute(rcpc::pc,
		std::span<const std::uint32_t>{stream}, cpu) ==
		gadgets::Event::system_call);
	test::require(cpu.registers[6] == 1);
}

} // namespace

int main()
{
	exhaustive_interpreter_offsets();
	interpreter_register_modes_constraints_and_faults();
	interpreter_store_invalidates_reservation();
	exhaustive_gadget_offsets_and_edges();
}
