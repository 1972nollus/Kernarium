// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_rcpc_test_support.hpp"

#include <array>
#include <cstring>

namespace rcpc = arm64_rcpc_test;
namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t raw = 0x8172'6354'4536'2788ULL;

void exhaustive_interpreter_widths_and_register_modes()
{
	for (const bool direct : {false, true}) {
		for (unsigned size_code = 0; size_code < 4; ++size_code) {
			const auto size = std::size_t{1} << size_code;
			for (const bool sp_base : {false, true}) {
				for (const bool discard : {false, true}) {
					rcpc::Memory memory;
					memory.direct = direct;
					memory.put(rcpc::base, raw, size);
					arm64::CpuState cpu{};
					test::fill_registers(cpu);
					cpu.pc = rcpc::pc;
					if (sp_base)
						cpu.sp = rcpc::base;
					else
						cpu.registers[2] = rcpc::base;
					const auto registers = cpu.registers;
					test::require(arm64::Dispatcher{}.execute(cpu, memory,
						rcpc::no_offset(size_code, sp_base ? 31U : 2U,
							discard ? 31U : 3U)).reason ==
						arm64::StopReason::continued);
					test::require(cpu.pc == rcpc::pc + 4);
					if (discard)
						test::require(cpu.registers == registers);
					else
						test::require(cpu.registers[3] ==
							(raw & rcpc::mask(size)));
					if (sp_base)
						test::require(cpu.sp == rcpc::base);
					test::require(memory.atomic_pointers == 1);
					if (direct)
						test::require(memory.reads == 0);
					else
						test::require(memory.reads == 1);
				}
			}
		}
	}
}

void interpreter_overlap_and_faults()
{
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		rcpc::Memory memory;
		memory.put(rcpc::base, raw, size);
		arm64::CpuState cpu{};
		cpu.pc = rcpc::pc;
		cpu.registers[2] = rcpc::base;
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			rcpc::no_offset(size_code, 2, 2)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[2] == (raw & rcpc::mask(size)));

		if (size > 1) {
			memory = {};
			cpu = {};
			cpu.pc = rcpc::pc;
			cpu.registers[2] = rcpc::base + 1;
			const auto before = cpu;
			const auto result = arm64::Dispatcher{}.execute(cpu, memory,
				rcpc::no_offset(size_code, 2, 3));
			test::require(result.reason == arm64::StopReason::memory_fault &&
				result.address == rcpc::base + 1 && !result.write);
			test::require(cpu.pc == before.pc &&
				cpu.registers == before.registers &&
				memory.atomic_pointers == 0 && memory.reads == 0);
		}
	}

	rcpc::Memory memory;
	memory.fault = true;
	arm64::CpuState cpu{};
	cpu.pc = rcpc::pc;
	cpu.registers[2] = rcpc::base;
	const auto before = cpu;
	const auto result = arm64::Dispatcher{}.execute(cpu, memory,
		rcpc::no_offset(3, 2, 3));
	test::require(result.reason == arm64::StopReason::memory_fault &&
		result.address == rcpc::base && !result.write);
	test::require(cpu.pc == before.pc && cpu.registers == before.registers);
}

void exhaustive_gadget_widths_and_edges()
{
	test::StaticMapping mapping{0x460'0000, 197,
		"/private/tmp/uml-base-rcpc-no-offset.XXXXXX"};
	const auto address = mapping.guest() + 4 * test::StaticMapping::page_size;
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		for (const bool sp_base : {false, true}) {
			for (const bool discard : {false, true}) {
				std::memcpy(mapping.at(address), &raw, size);
				arm64::CpuState cpu{};
				test::fill_registers(cpu);
				if (sp_base)
					cpu.sp = address;
				else
					cpu.registers[2] = address;
				const auto registers = cpu.registers;
				test::require(mapping.execute(rcpc::pc,
					rcpc::no_offset(size_code, sp_base ? 31U : 2U,
						discard ? 31U : 3U), cpu) ==
					gadgets::Event::system_call);
				if (discard)
					test::require(cpu.registers == registers);
				else
					test::require(cpu.registers[3] ==
						(raw & rcpc::mask(size)));
				if (sp_base)
					test::require(cpu.sp == address);
			}
		}

		std::memcpy(mapping.at(address), &raw, size);
		arm64::CpuState cpu{};
		cpu.registers[2] = address;
		test::require(mapping.execute(rcpc::pc,
			rcpc::no_offset(size_code, 2, 2), cpu) ==
			gadgets::Event::system_call);
		test::require(cpu.registers[2] == (raw & rcpc::mask(size)));

		if (size > 1) {
			cpu = {};
			cpu.registers[2] = address + 1;
			const auto before = cpu.registers;
			test::require(mapping.execute(rcpc::pc,
				rcpc::no_offset(size_code, 2, 3), cpu) ==
				gadgets::Event::memory_fault);
			test::require(cpu.pc == rcpc::pc && cpu.registers == before);
		}
	}

	arm64::CpuState cpu{};
	cpu.registers[2] = mapping.guest() + test::StaticMapping::length;
	const auto before = cpu.registers;
	test::require(mapping.execute(rcpc::pc,
		rcpc::no_offset(3, 2, 3), cpu) == gadgets::Event::memory_fault);
	test::require(cpu.pc == rcpc::pc && cpu.registers == before);
}

} // namespace

int main()
{
	exhaustive_interpreter_widths_and_register_modes();
	interpreter_overlap_and_faults();
	exhaustive_gadget_widths_and_edges();
}
