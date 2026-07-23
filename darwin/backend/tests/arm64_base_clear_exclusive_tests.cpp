// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_test_support.hpp"

#include <array>
#include <cstring>

namespace atomic_test = arm64_atomic_test;
namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t initial = 0xf1e2'd3c4'b5a6'9788ULL;
constexpr std::uint64_t replacement = 0x1021'3243'5465'7687ULL;

[[nodiscard]] constexpr std::uint32_t clrex(unsigned immediate)
{
	return 0xd503'305fU | ((immediate & 15U) << 8);
}

[[nodiscard]] constexpr std::uint32_t pair_exclusive(bool load,
	unsigned rn, unsigned rt, unsigned rt2, unsigned status = 31)
{
	return 0xc820'0000U | (static_cast<unsigned>(load) << 22) |
		((status & 31U) << 16) | ((rt2 & 31U) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

void exhaustive_interpreter_immediates()
{
	for (const bool direct : {false, true}) {
		for (unsigned immediate = 0; immediate < 16; ++immediate) {
			atomic_test::AtomicMemory memory;
			memory.direct = direct;
			memory.put(atomic_test::interpreter_base, initial, 8);
			arm64::CpuState cpu{};
			test::fill_registers(cpu);
			cpu.pc = atomic_test::pc;
			cpu.sp = 0xabc0;
			cpu.nzcv = 0xa000'0000U;
			cpu.registers[2] = atomic_test::interpreter_base;
			cpu.registers[3] = replacement;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				atomic_test::scalar_exclusive(3, true, 2, 4)).reason ==
				arm64::StopReason::continued);
			const auto registers = cpu.registers;
			const auto sp = cpu.sp;
			const auto nzcv = cpu.nzcv;
			const auto accesses = std::array{memory.atomic_pointers,
				memory.reads, memory.writes};
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				clrex(immediate)).reason == arm64::StopReason::continued);
			test::require(cpu.pc == atomic_test::pc + 8 &&
				cpu.registers == registers && cpu.sp == sp && cpu.nzcv == nzcv);
			test::require(cpu.exclusive_address == ~std::uint64_t{0} &&
				cpu.exclusive_key == 0 && cpu.exclusive_value == 0 &&
				cpu.exclusive_value2 == 0 && cpu.exclusive_version == 0 &&
				cpu.exclusive_size == 0);
			test::require(std::array{memory.atomic_pointers, memory.reads,
				memory.writes} == accesses);
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				atomic_test::scalar_exclusive(3, false, 2, 3, 5)).reason ==
				arm64::StopReason::continued);
			test::require(cpu.registers[5] == 1 &&
				memory.get(atomic_test::interpreter_base, 8) == initial);
		}
	}
}

void interpreter_clears_pair_monitor()
{
	atomic_test::AtomicMemory memory;
	const std::array values{initial, ~initial};
	memory.put_pair(atomic_test::interpreter_base, values);
	arm64::CpuState cpu{};
	cpu.pc = atomic_test::pc;
	cpu.registers[2] = atomic_test::interpreter_base;
	cpu.registers[5] = replacement;
	cpu.registers[6] = ~replacement;
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		pair_exclusive(true, 2, 3, 4)).reason ==
		arm64::StopReason::continued);
	test::require(arm64::Dispatcher{}.execute(cpu, memory, clrex(15)).reason ==
		arm64::StopReason::continued);
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		pair_exclusive(false, 2, 5, 6, 7)).reason ==
		arm64::StopReason::continued);
	test::require(cpu.registers[7] == 1 &&
		memory.get_pair(atomic_test::interpreter_base) == values);
}

void exhaustive_gadget_immediates()
{
	test::StaticMapping mapping{0x440'0000, 191,
		"/private/tmp/uml-base-clear-exclusive.XXXXXX"};
	const auto address = mapping.guest() + 4 * test::StaticMapping::page_size;

	/* Warm read and write translations before every monitor sequence. */
	arm64::CpuState warm{};
	warm.registers[2] = address;
	warm.registers[3] = initial;
	test::require(mapping.execute(atomic_test::pc,
		test::unsigned_immediate(3, 1, 0, 2, 8), warm) ==
		gadgets::Event::system_call);
	test::require(mapping.execute(atomic_test::pc,
		test::unsigned_immediate(3, 0, 0, 2, 3), warm) ==
		gadgets::Event::system_call);

	for (unsigned immediate = 0; immediate < 16; ++immediate) {
		std::memcpy(mapping.at(address), &initial, 8);
		arm64::CpuState cpu{};
		cpu.registers[2] = address;
		cpu.registers[3] = replacement;
		const std::array stream{
			atomic_test::scalar_exclusive(3, true, 2, 4),
			clrex(immediate),
			atomic_test::scalar_exclusive(3, false, 2, 3, 5),
			0xd400'0001U};
		test::require(mapping.execute(atomic_test::pc,
			std::span<const std::uint32_t>{stream}, cpu) ==
			gadgets::Event::system_call);
		std::uint64_t actual{};
		std::memcpy(&actual, mapping.at(address), 8);
		test::require(cpu.registers[5] == 1 && actual == initial);
	}
}

} // namespace

int main()
{
	exhaustive_interpreter_immediates();
	interpreter_clears_pair_monitor();
	exhaustive_gadget_immediates();
}
