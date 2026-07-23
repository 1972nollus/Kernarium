// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <set>

namespace atomic_test = arm64_atomic_test;
namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t observed_value = 0x7264'5648'3a2c'1e10ULL;
constexpr std::uint64_t operand_value = 0x6040'2000'8060'4020ULL;
constexpr std::uint32_t pass_flags = 0x2000'0000U;
constexpr std::uint32_t compare_fail_flags = 0xa000'0000U;

[[nodiscard]] constexpr std::uint32_t rcw_cas(bool soft, bool acquire,
	bool release, unsigned rn, unsigned rs, unsigned rt)
{
	return 0x1920'0800U | (static_cast<unsigned>(soft) << 30) |
		(static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) | ((rs & 31U) << 16) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t rcw_modify(bool soft, bool acquire,
	bool release, unsigned operation, unsigned rn, unsigned rs, unsigned rt)
{
	return 0x3820'8000U | (static_cast<unsigned>(soft) << 30) |
		(static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) |
		((operation & 7U) << 12) | ((rs & 31U) << 16) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t rcw(unsigned operation, bool soft,
	bool acquire, bool release, unsigned rn, unsigned source,
	unsigned result)
{
	return operation == 0U ?
		rcw_cas(soft, acquire, release, rn, result, source) :
		rcw_modify(soft, acquire, release, operation, rn, source, result);
}

[[nodiscard]] constexpr std::uint64_t desired(unsigned operation,
	std::uint64_t observed, std::uint64_t operand)
{
	switch (operation) {
	case 0: return operand;
	case 1: return observed & ~operand;
	case 2: return operand;
	case 3: return observed | operand;
	default: return observed;
	}
}

void prepare(arm64::CpuState &cpu, unsigned operation, unsigned rn,
	unsigned source, unsigned result, std::uint64_t address,
	std::uint64_t observed, std::uint64_t operand)
{
	test::fill_registers(cpu);
	cpu.pc = atomic_test::pc;
	if (rn == 31U)
		cpu.sp = address;
	else
		cpu.registers[rn] = address;
	if (source != 31U)
		cpu.registers[source] = operand;
	if (operation == 0U && result != 31U)
		cpu.registers[result] = observed;
}

void exhaustive_interpreter_matrix()
{
	for (const bool direct : {false, true}) {
		for (unsigned operation = 0; operation < 4; ++operation) {
			for (const bool soft : {false, true}) {
				for (const bool acquire : {false, true}) {
					for (const bool release : {false, true}) {
						for (const bool source_zero : {false, true}) {
							for (const bool result_zero : {false, true}) {
								for (const bool sp_base : {false, true}) {
									const unsigned rn = sp_base ? 31U : 2U;
									const unsigned source = source_zero ? 31U : 3U;
									const unsigned result = result_zero ? 31U : 4U;
									const auto observed = operation == 0U && result_zero ?
										0U : observed_value;
									const auto operand = source_zero ? 0U : operand_value;
									atomic_test::AtomicMemory memory;
									memory.direct = direct;
									memory.put(atomic_test::interpreter_base,
										observed, 8);
									arm64::CpuState cpu{};
									prepare(cpu, operation, rn, source, result,
										atomic_test::interpreter_base, observed,
										operand);
									const auto untouched = cpu.registers[6];
									test::require(arm64::Dispatcher{}.execute(cpu,
										memory, rcw(operation, soft, acquire,
											release, rn, source, result)).reason ==
										arm64::StopReason::continued);
									test::require(cpu.pc == atomic_test::pc + 4 &&
										cpu.sp == (sp_base ?
											atomic_test::interpreter_base : 0x4000) &&
										cpu.registers[6] == untouched &&
										cpu.nzcv == pass_flags);
									if (!result_zero)
										test::require(cpu.registers[result] == observed);
									test::require(memory.get(
										atomic_test::interpreter_base, 8) ==
										desired(operation, observed, operand));
									if (direct)
										test::require(memory.atomic_pointers != 0 &&
											memory.reads == 0 && memory.writes == 0);
									else
										test::require(memory.reads == 1 &&
											memory.writes == 1);
								}
							}
						}
					}
				}
			}
		}
	}
}

void interpreter_check_failures()
{
	for (const bool direct : {false, true}) {
		for (const bool soft : {false, true}) {
			atomic_test::AtomicMemory memory;
			memory.direct = direct;
			memory.put(atomic_test::interpreter_base, observed_value, 8);
			arm64::CpuState cpu{};
			prepare(cpu, 0, 2, 4, 3, atomic_test::interpreter_base,
				observed_value ^ 2U, operand_value);
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				rcw(0, soft, true, true, 2, 4, 3)).reason ==
				arm64::StopReason::continued);
			test::require(cpu.registers[3] == observed_value &&
				cpu.nzcv == compare_fail_flags &&
				memory.get(atomic_test::interpreter_base, 8) == observed_value);
		}

		for (unsigned operation = 0; operation < 4; ++operation) {
			const auto old = operation == 1U ? observed_value | 1U :
				observed_value;
			const auto operand = operation == 1U ? 1U : operand_value | 1U;
			atomic_test::AtomicMemory memory;
			memory.direct = direct;
			memory.put(atomic_test::interpreter_base, old, 8);
			arm64::CpuState cpu{};
			prepare(cpu, operation, 2, 4, 3,
				atomic_test::interpreter_base, old, operand);
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				rcw(operation, true, true, true, 2, 4, 3)).reason ==
				arm64::StopReason::continued);
			test::require(cpu.registers[3] == old && cpu.nzcv == 0 &&
				memory.get(atomic_test::interpreter_base, 8) == old);
		}
	}
}

void interpreter_faults_reserved_and_regressions()
{
	for (const bool direct : {false, true}) {
		atomic_test::AtomicMemory memory;
		memory.direct = direct;
		arm64::CpuState cpu{};
		prepare(cpu, 3, 2, 3, 4, atomic_test::interpreter_base + 1,
			observed_value, operand_value);
		const auto before = cpu;
		const auto alignment = arm64::Dispatcher{}.execute(cpu, memory,
			rcw(3, true, true, true, 2, 3, 4));
		test::require(alignment.reason == arm64::StopReason::memory_fault &&
			alignment.address == atomic_test::interpreter_base + 1 &&
			alignment.write && cpu.pc == before.pc &&
			cpu.registers == before.registers && cpu.nzcv == before.nzcv &&
			memory.atomic_pointers == 0 && memory.reads == 0 &&
			memory.writes == 0);

		memory = {};
		memory.direct = direct;
		memory.fault = true;
		cpu = {};
		prepare(cpu, 0, 2, 4, 3, atomic_test::interpreter_base,
			observed_value, operand_value);
		const auto fault_before = cpu;
		const auto fault = arm64::Dispatcher{}.execute(cpu, memory,
			rcw(0, false, true, true, 2, 4, 3));
		test::require(fault.reason == arm64::StopReason::memory_fault &&
			fault.address == atomic_test::interpreter_base && fault.write &&
			cpu.pc == fault_before.pc &&
			cpu.registers == fault_before.registers &&
			cpu.nzcv == fault_before.nzcv);
	}

	/* A fallback write fault occurs after the read, but must not commit any
	 * architectural register, flag, or PC state. */
	atomic_test::AtomicMemory write_fault;
	write_fault.direct = false;
	write_fault.fault_write = true;
	write_fault.put(atomic_test::interpreter_base, observed_value, 8);
	arm64::CpuState cpu{};
	prepare(cpu, 2, 2, 3, 4, atomic_test::interpreter_base,
		observed_value, operand_value);
	const auto before_write_fault = cpu;
	const auto failed_write = arm64::Dispatcher{}.execute(cpu, write_fault,
		rcw(2, true, true, true, 2, 3, 4));
	test::require(failed_write.reason == arm64::StopReason::memory_fault &&
		cpu.pc == before_write_fault.pc &&
		cpu.registers == before_write_fault.registers &&
		cpu.nzcv == before_write_fault.nzcv && write_fault.reads == 1 &&
		write_fault.writes == 1 &&
		write_fault.get(atomic_test::interpreter_base, 8) == observed_value);

	for (unsigned operation = 4; operation < 8; ++operation) {
		atomic_test::AtomicMemory memory;
		arm64::CpuState invalid_cpu{};
		invalid_cpu.pc = atomic_test::pc;
		const auto instruction = rcw_modify(false, false, false,
			operation, 2, 3, 4);
		test::require(arm64::Dispatcher{}.execute(invalid_cpu, memory,
			instruction).reason == arm64::StopReason::illegal_instruction);
		test::require(invalid_cpu.pc == atomic_test::pc &&
			memory.atomic_pointers == 0 && memory.reads == 0 &&
			memory.writes == 0);
	}
	/* opcode zero is the existing SWPB/SWPH allocation. RCW decoding must not
	 * steal either byte or halfword from ordinary FEAT_LSE. */
	for (unsigned size_code = 0; size_code < 2; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		atomic_test::AtomicMemory memory;
		memory.put(atomic_test::interpreter_base, observed_value, size);
		arm64::CpuState swap_cpu{};
		swap_cpu.pc = atomic_test::pc;
		swap_cpu.registers[2] = atomic_test::interpreter_base;
		swap_cpu.registers[3] = operand_value;
		test::require(arm64::Dispatcher{}.execute(swap_cpu, memory,
			atomic_test::atomic(size_code, false, false, 8, 2, 3, 4)).reason ==
			arm64::StopReason::continued);
		test::require(swap_cpu.registers[4] ==
			(observed_value & atomic_test::mask_for_size(size)) &&
			memory.get(atomic_test::interpreter_base, size) ==
			(operand_value & atomic_test::mask_for_size(size)));
	}
}

void interpreter_register_overlaps_and_reservations()
{
	for (unsigned operation = 0; operation < 4; ++operation) {
		/* Rn==result: capture the address before returning the old value. */
		atomic_test::AtomicMemory memory;
		memory.put(atomic_test::interpreter_base,
			operation == 0U ? atomic_test::interpreter_base : observed_value, 8);
		arm64::CpuState cpu{};
		cpu.pc = atomic_test::pc;
		cpu.registers[2] = atomic_test::interpreter_base;
		cpu.registers[3] = operand_value;
		const auto old = operation == 0U ?
			atomic_test::interpreter_base : observed_value;
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			rcw(operation, false, false, false, 2, 3, 2)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[2] == old &&
			memory.get(atomic_test::interpreter_base, 8) ==
			desired(operation, old, operand_value));

		/* source==result: both input roles are captured before writeback. */
		const auto shared = operation == 0U ? observed_value : operand_value;
		memory = {};
		memory.put(atomic_test::interpreter_base, observed_value, 8);
		cpu = {};
		cpu.pc = atomic_test::pc;
		cpu.registers[2] = atomic_test::interpreter_base;
		cpu.registers[3] = shared;
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			rcw(operation, false, false, false, 2, 3, 3)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[3] == observed_value &&
			memory.get(atomic_test::interpreter_base, 8) ==
			desired(operation, observed_value, shared));
	}

	for (const bool direct : {false, true}) {
		for (const bool compare_failure : {false, true}) {
			atomic_test::AtomicMemory memory;
			memory.direct = direct;
			const auto old = compare_failure ? observed_value :
				(observed_value | 1U);
			memory.put(atomic_test::interpreter_base, old, 8);
			arm64::CpuState cpu{};
			cpu.pc = atomic_test::pc;
			cpu.registers[2] = atomic_test::interpreter_base;
			cpu.registers[3] = compare_failure ? old ^ 2U : old;
			cpu.registers[4] = compare_failure ? operand_value : old & ~1U;
			cpu.registers[6] = ~old;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				atomic_test::scalar_exclusive(3, true, 2, 5)).reason ==
				arm64::StopReason::continued);
			const auto instruction = compare_failure ?
				rcw(0, false, false, false, 2, 4, 3) :
				rcw(0, true, false, false, 2, 4, 3);
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				instruction).reason == arm64::StopReason::continued);
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				atomic_test::scalar_exclusive(3, false, 2, 6, 7)).reason ==
				arm64::StopReason::continued);
			test::require(cpu.registers[7] == 1 &&
				memory.get(atomic_test::interpreter_base, 8) == old);
		}
	}
}

void run_gadget_case(test::StaticMapping &mapping, std::uint64_t address,
	unsigned operation, bool soft, bool acquire, bool release,
	bool source_zero, bool result_zero, bool sp_base)
{
	const unsigned rn = sp_base ? 31U : 2U;
	const unsigned source = source_zero ? 31U : 3U;
	const unsigned result = result_zero ? 31U : 4U;
	const auto observed = operation == 0U && result_zero ? 0U : observed_value;
	const auto operand = source_zero ? 0U : operand_value;
	std::memcpy(mapping.at(address), &observed, 8);
	arm64::CpuState cpu{};
	prepare(cpu, operation, rn, source, result, address, observed, operand);
	const auto untouched = cpu.registers[6];
	test::require(mapping.execute(atomic_test::pc,
		rcw(operation, soft, acquire, release, rn, source, result), cpu) ==
		gadgets::Event::system_call);
	std::uint64_t actual{};
	std::memcpy(&actual, mapping.at(address), 8);
	test::require(actual == desired(operation, observed, operand) &&
		cpu.nzcv == pass_flags && cpu.registers[6] == untouched &&
		cpu.sp == (sp_base ? address : 0x4000));
	if (!result_zero)
		test::require(cpu.registers[result] == observed);
}

void exhaustive_gadget_matrix_and_edges()
{
	test::StaticMapping mapping{0x4b0'0000, 241,
		"/private/tmp/uml-base-rcw-scalar.XXXXXX"};
	const auto address = mapping.guest() + 4 * test::StaticMapping::page_size;
	for (unsigned operation = 0; operation < 4; ++operation)
		for (const bool soft : {false, true})
			for (const bool acquire : {false, true})
				for (const bool release : {false, true})
					for (const bool source_zero : {false, true})
						for (const bool result_zero : {false, true})
							for (const bool sp_base : {false, true})
								run_gadget_case(mapping, address, operation,
									soft, acquire, release, source_zero,
									result_zero, sp_base);

	for (const bool soft : {false, true}) {
		std::memcpy(mapping.at(address), &observed_value, 8);
		arm64::CpuState cpu{};
		prepare(cpu, 0, 2, 4, 3, address, observed_value ^ 2U,
			operand_value);
		test::require(mapping.execute(atomic_test::pc,
			rcw(0, soft, true, true, 2, 4, 3), cpu) ==
			gadgets::Event::system_call);
		std::uint64_t actual{};
		std::memcpy(&actual, mapping.at(address), 8);
		test::require(actual == observed_value &&
			cpu.registers[3] == observed_value &&
			cpu.nzcv == compare_fail_flags);
	}
	for (unsigned operation = 0; operation < 4; ++operation) {
		const auto old = operation == 1U ? observed_value | 1U :
			observed_value;
		const auto operand = operation == 1U ? 1U : operand_value | 1U;
		std::memcpy(mapping.at(address), &old, 8);
		arm64::CpuState cpu{};
		prepare(cpu, operation, 2, 4, 3, address, old, operand);
		test::require(mapping.execute(atomic_test::pc,
			rcw(operation, true, true, true, 2, 4, 3), cpu) ==
			gadgets::Event::system_call);
		std::uint64_t actual{};
		std::memcpy(&actual, mapping.at(address), 8);
		test::require(actual == old && cpu.registers[3] == old &&
			cpu.nzcv == 0);
	}

	arm64::CpuState cpu{};
	prepare(cpu, 3, 2, 3, 4, address + 1, observed_value, operand_value);
	const auto before = cpu;
	test::require(mapping.execute(atomic_test::pc,
		rcw(3, true, true, true, 2, 3, 4), cpu) ==
		gadgets::Event::memory_fault);
	test::require(cpu.pc == atomic_test::pc &&
		cpu.registers == before.registers && cpu.nzcv == before.nzcv);
	cpu = {};
	prepare(cpu, 0, 2, 4, 3,
		mapping.guest() + test::StaticMapping::length,
		observed_value, operand_value);
	const auto unmapped_before = cpu;
	test::require(mapping.execute(atomic_test::pc,
		rcw(0, false, true, true, 2, 4, 3), cpu) ==
		gadgets::Event::memory_fault);
	test::require(cpu.pc == atomic_test::pc &&
		cpu.registers == unmapped_before.registers &&
		cpu.nzcv == unmapped_before.nzcv);

	for (unsigned operation = 4; operation < 8; ++operation) {
		cpu = {};
		test::require(mapping.execute(atomic_test::pc,
			rcw_modify(false, false, false, operation, 2, 3, 4), cpu) ==
			gadgets::Event::illegal_instruction);
	}
	for (unsigned size_code = 0; size_code < 2; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		std::memcpy(mapping.at(address), &observed_value, size);
		cpu = {};
		cpu.registers[2] = address;
		cpu.registers[3] = operand_value;
		test::require(mapping.execute(atomic_test::pc,
			atomic_test::atomic(size_code, false, false, 8, 2, 3, 4), cpu) ==
			gadgets::Event::system_call);
		std::uint64_t actual{};
		std::memcpy(&actual, mapping.at(address), size);
		test::require(cpu.registers[4] ==
			(observed_value & atomic_test::mask_for_size(size)) && actual ==
			(operand_value & atomic_test::mask_for_size(size)));
	}

	/* Warm both permissions before establishing the host monitor. Both a
	 * failed compare and a failed software check still write back the old
	 * value and therefore invalidate the guest reservation. */
	arm64::CpuState warm{};
	warm.registers[2] = address;
	warm.registers[3] = observed_value;
	test::require(mapping.execute(atomic_test::pc,
		test::unsigned_immediate(3, 1, 0, 2, 8), warm) ==
		gadgets::Event::system_call);
	test::require(mapping.execute(atomic_test::pc,
		test::unsigned_immediate(3, 0, 0, 2, 3), warm) ==
		gadgets::Event::system_call);
	for (const bool compare_failure : {false, true}) {
		const auto old = compare_failure ? observed_value : observed_value | 1U;
		std::memcpy(mapping.at(address), &old, 8);
		cpu = {};
		cpu.registers[2] = address;
		cpu.registers[3] = compare_failure ? old ^ 2U : old;
		cpu.registers[4] = compare_failure ? operand_value : old & ~1U;
		cpu.registers[6] = ~old;
		const auto check = compare_failure ?
			rcw(0, false, false, false, 2, 4, 3) :
			rcw(0, true, false, false, 2, 4, 3);
		const std::array stream{
			atomic_test::scalar_exclusive(3, true, 2, 5), check,
			atomic_test::scalar_exclusive(3, false, 2, 6, 7),
			0xd400'0001U};
		test::require(mapping.execute(atomic_test::pc,
			std::span<const std::uint32_t>{stream}, cpu) ==
			gadgets::Event::system_call);
		test::require(cpu.registers[7] == 1);
	}
}

void gadget_register_overlaps()
{
	test::StaticMapping mapping{0x4c0'0000, 243,
		"/private/tmp/uml-base-rcw-overlap.XXXXXX"};
	const auto address = mapping.guest() + 4 * test::StaticMapping::page_size;
	for (unsigned operation = 0; operation < 4; ++operation) {
		const auto old = operation == 0U ? address : observed_value;
		std::memcpy(mapping.at(address), &old, 8);
		arm64::CpuState cpu{};
		cpu.registers[2] = address;
		cpu.registers[3] = operand_value;
		test::require(mapping.execute(atomic_test::pc,
			rcw(operation, false, false, false, 2, 3, 2), cpu) ==
			gadgets::Event::system_call);
		std::uint64_t actual{};
		std::memcpy(&actual, mapping.at(address), 8);
		test::require(cpu.registers[2] == old &&
			actual == desired(operation, old, operand_value));

		const auto shared = operation == 0U ? observed_value : operand_value;
		std::memcpy(mapping.at(address), &observed_value, 8);
		cpu = {};
		cpu.registers[2] = address;
		cpu.registers[3] = shared;
		test::require(mapping.execute(atomic_test::pc,
			rcw(operation, false, false, false, 2, 3, 3), cpu) ==
			gadgets::Event::system_call);
		std::memcpy(&actual, mapping.at(address), 8);
		test::require(cpu.registers[3] == observed_value &&
			actual == desired(operation, observed_value, shared));
	}
}

void production_gadget_specialization()
{
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
	return;
#else
	std::set<std::uintptr_t> entries;
	for (unsigned operation = 0; operation < 4; ++operation) {
		for (const bool soft : {false, true}) {
			for (const bool acquire : {false, true}) {
				for (const bool release : {false, true}) {
					for (const bool source_zero : {false, true}) {
						for (const bool result_zero : {false, true}) {
							const auto built = gadgets::Builder{}.build(
								atomic_test::pc,
								std::array{rcw(operation, soft, acquire,
									release, 2, source_zero ? 31U : 3U,
									result_zero ? 31U : 4U)});
							test::require(built.complete &&
								!built.program.words().empty());
							entries.insert(built.program.words().front());
						}
					}
				}
			}
		}
	}
	/* Acquire is architecturally suppressed when the result is XZR, so those
	 * 32 encodings deliberately reuse their relaxed/release gadget. */
	test::require(entries.size() == 96);
	for (unsigned operation = 0; operation < 4; ++operation)
		for (const bool soft : {false, true})
			for (const bool release : {false, true})
				for (const bool source_zero : {false, true}) {
					const auto source = source_zero ? 31U : 3U;
					const auto relaxed = gadgets::Builder{}.build(atomic_test::pc,
						std::array{rcw(operation, soft, false, release,
							2, source, 31)});
					const auto suppressed = gadgets::Builder{}.build(atomic_test::pc,
						std::array{rcw(operation, soft, true, release,
							2, source, 31)});
					test::require(relaxed.program.words().front() ==
						suppressed.program.words().front());
				}
#endif
}

} // namespace

int main()
{
	exhaustive_interpreter_matrix();
	interpreter_check_failures();
	interpreter_faults_reserved_and_regressions();
	interpreter_register_overlaps_and_reservations();
	exhaustive_gadget_matrix_and_edges();
	gadget_register_overlaps();
	production_gadget_specialization();
}
