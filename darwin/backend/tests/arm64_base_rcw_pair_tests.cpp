// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_test_support.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <set>
#include <thread>

namespace atomic_test = arm64_atomic_test;
namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::array initial{
	0x7264'5648'3a2c'1e10ULL, 0xf1e2'd3c4'b5a6'9788ULL};
constexpr std::array operand{
	0x6040'2000'8060'4020ULL, 0x0f1e'2d3c'4b5a'6978ULL};
constexpr std::uint32_t pass_flags = 0x2000'0000U;
constexpr std::uint32_t compare_fail_flags = 0xa000'0000U;

[[nodiscard]] constexpr std::uint32_t rcw_cas_pair(bool soft, bool acquire,
	bool release, unsigned rn, unsigned compare, unsigned replacement)
{
	return 0x1920'0c00U | (static_cast<unsigned>(soft) << 30) |
		(static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) |
		((compare & 31U) << 16) | ((rn & 31U) << 5) |
		(replacement & 31U);
}

[[nodiscard]] constexpr std::uint32_t rcw_modify_pair(bool soft,
	bool acquire, bool release, unsigned operation, unsigned rn,
	unsigned first, unsigned second)
{
	return 0x1920'8000U | (static_cast<unsigned>(soft) << 30) |
		(static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) |
		((second & 31U) << 16) | ((operation & 7U) << 12) |
		((rn & 31U) << 5) | (first & 31U);
}

[[nodiscard]] constexpr std::array<std::uint64_t, 2> desired(
	unsigned operation, const std::array<std::uint64_t, 2> &old,
	const std::array<std::uint64_t, 2> &source)
{
	if (operation == 0U || operation == 2U)
		return source;
	if (operation == 1U)
		return {old[0] & ~source[0], old[1] & ~source[1]};
	return {old[0] | source[0], old[1] | source[1]};
}

void set_pair_registers(arm64::CpuState &cpu, unsigned first,
	const std::array<std::uint64_t, 2> &values)
{
	cpu.registers[first] = values[0];
	if (first + 1U != 31U)
		cpu.registers[first + 1U] = values[1];
}

void prepare_cas(arm64::CpuState &cpu, unsigned rn, unsigned compare,
	unsigned replacement, std::uint64_t address,
	const std::array<std::uint64_t, 2> &old,
	const std::array<std::uint64_t, 2> &source)
{
	test::fill_registers(cpu);
	cpu.pc = atomic_test::pc;
	if (rn == 31U)
		cpu.sp = address;
	else
		cpu.registers[rn] = address;
	set_pair_registers(cpu, compare, old);
	set_pair_registers(cpu, replacement, source);
}

void prepare_modify(arm64::CpuState &cpu, unsigned rn, unsigned first,
	unsigned second, std::uint64_t address,
	const std::array<std::uint64_t, 2> &source)
{
	test::fill_registers(cpu);
	cpu.pc = atomic_test::pc;
	if (rn == 31U)
		cpu.sp = address;
	else
		cpu.registers[rn] = address;
	cpu.registers[first] = source[0];
	cpu.registers[second] = source[1];
}

void exhaustive_interpreter_matrix()
{
	for (const bool direct : {false, true}) {
		for (unsigned operation = 0; operation < 4; ++operation) {
			for (const bool soft : {false, true}) {
				for (const bool acquire : {false, true}) {
					for (const bool release : {false, true}) {
						for (const bool sp_base : {false, true}) {
							const unsigned rn = sp_base ? 31U : 6U;
							const unsigned mode_count = operation == 0U ? 4U : 1U;
							for (unsigned mode = 0; mode < mode_count; ++mode) {
								const unsigned replacement =
									(mode & 2U) != 0 ? 30U : 4U;
								const unsigned result =
									(mode & 1U) != 0 ? 30U : 2U;
								auto old = initial;
								auto source = operand;
								if (result == 30U)
									old[1] = 0;
								if (replacement == 30U)
									source[1] = 0;
								if (replacement == result)
									source[0] = old[0];
								atomic_test::AtomicMemory memory;
								memory.direct = direct;
								memory.put_pair(atomic_test::interpreter_base, old);
								arm64::CpuState cpu{};
								std::uint32_t instruction;
								if (operation == 0U) {
									prepare_cas(cpu, rn, result, replacement,
										atomic_test::interpreter_base, old, source);
									instruction = rcw_cas_pair(soft, acquire,
										release, rn, result, replacement);
								} else {
									prepare_modify(cpu, rn, 3, 4,
										atomic_test::interpreter_base, source);
									instruction = rcw_modify_pair(soft, acquire,
										release, operation, rn, 3, 4);
								}
								const auto untouched = cpu.registers[7];
								test::require(arm64::Dispatcher{}.execute(cpu,
									memory, instruction).reason ==
									arm64::StopReason::continued);
								test::require(cpu.pc == atomic_test::pc + 4 &&
									cpu.nzcv == pass_flags &&
									cpu.sp == (sp_base ?
										atomic_test::interpreter_base : 0x4000) &&
									cpu.registers[7] == untouched);
								if (operation == 0U) {
									test::require(cpu.registers[result] == old[0]);
									if (result != 30U)
										test::require(cpu.registers[result + 1U] == old[1]);
								} else {
									test::require(cpu.registers[3] == old[0] &&
										cpu.registers[4] == old[1]);
								}
								test::require(memory.get_pair(
									atomic_test::interpreter_base) ==
									desired(operation, old, source));
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

void interpreter_outcomes_faults_and_constraints()
{
	for (const bool direct : {false, true}) {
		for (const bool soft : {false, true}) {
			for (unsigned mismatch = 0; mismatch < 2; ++mismatch) {
				atomic_test::AtomicMemory memory;
				memory.direct = direct;
				memory.put_pair(atomic_test::interpreter_base, initial);
				auto compare = initial;
				compare[mismatch] ^= 2U;
				arm64::CpuState cpu{};
				prepare_cas(cpu, 6, 2, 4, atomic_test::interpreter_base,
					compare, operand);
				test::require(arm64::Dispatcher{}.execute(cpu, memory,
					rcw_cas_pair(soft, true, true, 6, 2, 4)).reason ==
					arm64::StopReason::continued);
				test::require(cpu.registers[2] == initial[0] &&
					cpu.registers[3] == initial[1] &&
					cpu.nzcv == compare_fail_flags &&
					memory.get_pair(atomic_test::interpreter_base) == initial);
			}
		}

		for (unsigned operation = 0; operation < 4; ++operation) {
			auto old = initial;
			auto source = operand;
			if (operation == 1U) {
				old[0] |= 1U;
				source = {1U, 0U};
			} else {
				source[0] |= 1U;
			}
			atomic_test::AtomicMemory memory;
			memory.direct = direct;
			memory.put_pair(atomic_test::interpreter_base, old);
			arm64::CpuState cpu{};
			const auto instruction = operation == 0U ?
				rcw_cas_pair(true, true, true, 6, 2, 4) :
				rcw_modify_pair(true, true, true, operation, 6, 3, 4);
			if (operation == 0U)
				prepare_cas(cpu, 6, 2, 4, atomic_test::interpreter_base,
					old, source);
			else
				prepare_modify(cpu, 6, 3, 4,
					atomic_test::interpreter_base, source);
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				instruction).reason == arm64::StopReason::continued);
			test::require(cpu.nzcv == 0 &&
				memory.get_pair(atomic_test::interpreter_base) == old);
		}
	}

	/* Changing only the high doubleword is allowed by the remaining userspace
	 * RCWS state check; the protected bit is bit zero of the full 128-bit value. */
	for (unsigned operation = 0; operation < 4; ++operation) {
		auto source = initial;
		if (operation == 1U)
			source = {0U, 1U};
		else
			source[1] ^= 1U;
		atomic_test::AtomicMemory memory;
		memory.put_pair(atomic_test::interpreter_base, initial);
		arm64::CpuState cpu{};
		const auto instruction = operation == 0U ?
			rcw_cas_pair(true, false, false, 6, 2, 4) :
			rcw_modify_pair(true, false, false, operation, 6, 3, 4);
		if (operation == 0U)
			prepare_cas(cpu, 6, 2, 4, atomic_test::interpreter_base,
				initial, source);
		else
			prepare_modify(cpu, 6, 3, 4,
				atomic_test::interpreter_base, source);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			instruction).reason == arm64::StopReason::continued);
		test::require(cpu.nzcv == pass_flags &&
			memory.get_pair(atomic_test::interpreter_base) ==
			desired(operation, initial, source));
	}

	const auto require_illegal = [](std::uint32_t instruction) {
		atomic_test::AtomicMemory memory;
		arm64::CpuState cpu{};
		cpu.pc = atomic_test::pc;
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			instruction).reason == arm64::StopReason::illegal_instruction);
		test::require(cpu.pc == atomic_test::pc &&
			memory.atomic_pointers == 0 && memory.reads == 0 &&
			memory.writes == 0);
	};
	for (const auto instruction : {
		rcw_cas_pair(false, false, false, 6, 3, 4),
		rcw_cas_pair(false, false, false, 6, 2, 5),
		rcw_modify_pair(false, false, false, 1, 6, 31, 4),
		rcw_modify_pair(false, false, false, 1, 6, 3, 31),
		rcw_modify_pair(false, false, false, 1, 6, 3, 3)})
		require_illegal(instruction);
	for (unsigned operation = 4; operation < 8; ++operation)
		require_illegal(rcw_modify_pair(false, false, false,
			operation, 6, 3, 4));

	atomic_test::AtomicMemory memory;
	arm64::CpuState cpu{};
	prepare_cas(cpu, 6, 2, 4, atomic_test::interpreter_base + 8,
		initial, operand);
	const auto before = cpu;
	const auto alignment = arm64::Dispatcher{}.execute(cpu, memory,
		rcw_cas_pair(false, true, true, 6, 2, 4));
	test::require(alignment.reason == arm64::StopReason::memory_fault &&
		alignment.address == atomic_test::interpreter_base + 8 &&
		alignment.write && cpu.pc == before.pc &&
		cpu.registers == before.registers && cpu.nzcv == before.nzcv &&
		memory.atomic_pointers == 0);

	memory = {};
	memory.fault = true;
	cpu = {};
	prepare_modify(cpu, 6, 3, 4, atomic_test::interpreter_base, operand);
	const auto fault_before = cpu;
	const auto fault = arm64::Dispatcher{}.execute(cpu, memory,
		rcw_modify_pair(true, true, true, 3, 6, 3, 4));
	test::require(fault.reason == arm64::StopReason::memory_fault &&
		fault.address == atomic_test::interpreter_base && fault.write &&
		cpu.pc == fault_before.pc &&
		cpu.registers == fault_before.registers &&
		cpu.nzcv == fault_before.nzcv);

	memory = {};
	memory.direct = false;
	memory.fault_write = true;
	memory.put_pair(atomic_test::interpreter_base, initial);
	cpu = {};
	prepare_modify(cpu, 6, 3, 4, atomic_test::interpreter_base, operand);
	const auto write_before = cpu;
	const auto write_fault = arm64::Dispatcher{}.execute(cpu, memory,
		rcw_modify_pair(true, true, true, 2, 6, 3, 4));
	test::require(write_fault.reason == arm64::StopReason::memory_fault &&
		cpu.pc == write_before.pc && cpu.registers == write_before.registers &&
		cpu.nzcv == write_before.nzcv && memory.reads == 1 &&
		memory.writes == 1 &&
		memory.get_pair(atomic_test::interpreter_base) == initial);
}

void interpreter_overlaps_reservations_and_atomicity()
{
	/* Pair CAS captures base, comparisons, and replacements before result
	 * writeback, including fully overlapping compare/replacement pairs. */
	for (const bool base_is_compare : {false, true}) {
		auto old = initial;
		auto source = operand;
		const unsigned rn = base_is_compare ? 2U : 4U;
		if (base_is_compare)
			old[0] = atomic_test::interpreter_base;
		else
			source[0] = atomic_test::interpreter_base;
		atomic_test::AtomicMemory memory;
		memory.put_pair(atomic_test::interpreter_base, old);
		arm64::CpuState cpu{};
		prepare_cas(cpu, rn, 2, 4, atomic_test::interpreter_base,
			old, source);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			rcw_cas_pair(false, false, false, rn, 2, 4)).reason ==
			arm64::StopReason::continued);
		test::require(memory.get_pair(atomic_test::interpreter_base) == source &&
			cpu.registers[2] == old[0] && cpu.registers[3] == old[1]);
	}
	{
		atomic_test::AtomicMemory memory;
		memory.put_pair(atomic_test::interpreter_base, initial);
		arm64::CpuState cpu{};
		prepare_cas(cpu, 6, 2, 2, atomic_test::interpreter_base,
			initial, initial);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			rcw_cas_pair(false, false, false, 6, 2, 2)).reason ==
			arm64::StopReason::continued);
		test::require(memory.get_pair(atomic_test::interpreter_base) == initial &&
			cpu.registers[2] == initial[0] && cpu.registers[3] == initial[1]);
	}
	for (const bool base_is_second : {false, true}) {
		auto source = operand;
		const unsigned rn = base_is_second ? 4U : 3U;
		source[base_is_second ? 1U : 0U] = atomic_test::interpreter_base;
		atomic_test::AtomicMemory memory;
		memory.put_pair(atomic_test::interpreter_base, initial);
		arm64::CpuState cpu{};
		prepare_modify(cpu, rn, 3, 4, atomic_test::interpreter_base, source);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			rcw_modify_pair(false, false, false, 3, rn, 3, 4)).reason ==
			arm64::StopReason::continued);
		test::require(memory.get_pair(atomic_test::interpreter_base) ==
			desired(3, initial, source) && cpu.registers[3] == initial[0] &&
			cpu.registers[4] == initial[1]);
	}

	for (const bool direct : {false, true}) {
		for (const bool compare_failure : {false, true}) {
			atomic_test::AtomicMemory memory;
			memory.direct = direct;
			auto old = initial;
			auto compare = old;
			auto replacement = operand;
			if (compare_failure)
				compare[1] ^= 2U;
			else {
				old[0] |= 1U;
				compare = old;
				replacement = {old[0] & ~1U, old[1]};
			}
			memory.put_pair(atomic_test::interpreter_base, old);
			arm64::CpuState cpu{};
			prepare_cas(cpu, 6, 2, 4, atomic_test::interpreter_base,
				compare, replacement);
			cpu.registers[8] = ~old[0];
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				atomic_test::scalar_exclusive(3, true, 6, 7)).reason ==
				arm64::StopReason::continued);
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				rcw_cas_pair(!compare_failure, false, false,
					6, 2, 4)).reason == arm64::StopReason::continued);
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				atomic_test::scalar_exclusive(3, false, 6, 8, 9)).reason ==
				arm64::StopReason::continued);
			test::require(cpu.registers[9] == 1 &&
				memory.get_pair(atomic_test::interpreter_base) == old);
		}
	}

	atomic_test::AtomicMemory memory;
	memory.put_pair(atomic_test::interpreter_base, initial);
	auto *host = memory.host_pair(atomic_test::interpreter_base);
	std::atomic<bool> running{true};
	std::atomic<bool> ready{false};
	std::thread writer{[&] {
		ready.store(true, std::memory_order_release);
		while (running.load(std::memory_order_acquire)) {
			atomic_test::native_store_pair(host, operand);
			atomic_test::native_store_pair(host, initial);
		}
	}};
	while (!ready.load(std::memory_order_acquire))
		std::this_thread::yield();
	for (unsigned iteration = 0; iteration < 5000; ++iteration) {
		arm64::CpuState cpu{};
		prepare_modify(cpu, 6, 3, 4, atomic_test::interpreter_base,
			{0U, 0U});
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			rcw_modify_pair(false, true, true, 1, 6, 3, 4)).reason ==
			arm64::StopReason::continued);
		const std::array observed{cpu.registers[3], cpu.registers[4]};
		test::require(observed == initial || observed == operand);
	}
	running.store(false, std::memory_order_release);
	writer.join();

	memory.put_pair(atomic_test::interpreter_base, initial);
	running.store(true, std::memory_order_relaxed);
	ready.store(false, std::memory_order_relaxed);
	std::atomic<bool> torn{false};
	std::thread reader{[&] {
		ready.store(true, std::memory_order_release);
		while (running.load(std::memory_order_acquire)) {
			const auto pair = atomic_test::native_load_pair(host);
			if (pair != initial && pair != operand)
				torn.store(true, std::memory_order_relaxed);
		}
	}};
	while (!ready.load(std::memory_order_acquire))
		std::this_thread::yield();
	for (unsigned iteration = 0; iteration < 5000; ++iteration) {
		const auto &value = iteration & 1U ? initial : operand;
		arm64::CpuState cpu{};
		prepare_modify(cpu, 6, 3, 4, atomic_test::interpreter_base, value);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			rcw_modify_pair(false, true, true, 2, 6, 3, 4)).reason ==
			arm64::StopReason::continued);
	}
	running.store(false, std::memory_order_release);
	reader.join();
	test::require(!torn.load(std::memory_order_relaxed));
}

void exhaustive_gadget_matrix_and_edges()
{
	test::StaticMapping mapping{0x4d0'0000, 245,
		"/private/tmp/uml-base-rcw-pair.XXXXXX"};
	const auto address = mapping.guest() + 4 * test::StaticMapping::page_size;
	for (unsigned operation = 0; operation < 4; ++operation) {
		for (const bool soft : {false, true}) {
			for (const bool acquire : {false, true}) {
				for (const bool release : {false, true}) {
					for (const bool sp_base : {false, true}) {
						const unsigned rn = sp_base ? 31U : 6U;
						const unsigned modes = operation == 0U ? 4U : 1U;
						for (unsigned mode = 0; mode < modes; ++mode) {
							const unsigned replacement =
								(mode & 2U) != 0 ? 30U : 4U;
							const unsigned result =
								(mode & 1U) != 0 ? 30U : 2U;
							auto old = initial;
							auto source = operand;
							if (result == 30U)
								old[1] = 0;
							if (replacement == 30U)
								source[1] = 0;
							if (replacement == result)
								source[0] = old[0];
							std::memcpy(mapping.at(address), old.data(), 16);
							arm64::CpuState cpu{};
							std::uint32_t instruction;
							if (operation == 0U) {
								prepare_cas(cpu, rn, result, replacement,
									address, old, source);
								instruction = rcw_cas_pair(soft, acquire,
									release, rn, result, replacement);
							} else {
								prepare_modify(cpu, rn, 3, 4, address, source);
								instruction = rcw_modify_pair(soft, acquire,
									release, operation, rn, 3, 4);
							}
							test::require(mapping.execute(atomic_test::pc,
								instruction, cpu) == gadgets::Event::system_call);
							std::array<std::uint64_t, 2> actual{};
							std::memcpy(actual.data(), mapping.at(address), 16);
							test::require(actual == desired(operation, old, source) &&
								cpu.nzcv == pass_flags &&
								cpu.sp == (sp_base ? address : 0x4000));
							if (operation == 0U) {
								test::require(cpu.registers[result] == old[0]);
								if (result != 30U)
									test::require(cpu.registers[result + 1U] == old[1]);
							} else {
								test::require(cpu.registers[3] == old[0] &&
									cpu.registers[4] == old[1]);
							}
						}
					}
				}
			}
		}
	}

	for (const bool soft : {false, true}) {
		std::memcpy(mapping.at(address), initial.data(), 16);
		auto compare = initial;
		compare[1] ^= 2U;
		arm64::CpuState cpu{};
		prepare_cas(cpu, 6, 2, 4, address, compare, operand);
		test::require(mapping.execute(atomic_test::pc,
			rcw_cas_pair(soft, true, true, 6, 2, 4), cpu) ==
			gadgets::Event::system_call);
		std::array<std::uint64_t, 2> actual{};
		std::memcpy(actual.data(), mapping.at(address), 16);
		test::require(actual == initial && cpu.nzcv == compare_fail_flags &&
			cpu.registers[2] == initial[0] && cpu.registers[3] == initial[1]);
	}
	for (unsigned operation = 0; operation < 4; ++operation) {
		auto old = initial;
		auto source = operand;
		if (operation == 1U) {
			old[0] |= 1U;
			source = {1U, 0U};
		} else {
			source[0] |= 1U;
		}
		std::memcpy(mapping.at(address), old.data(), 16);
		arm64::CpuState cpu{};
		const auto instruction = operation == 0U ?
			rcw_cas_pair(true, true, true, 6, 2, 4) :
			rcw_modify_pair(true, true, true, operation, 6, 3, 4);
		if (operation == 0U)
			prepare_cas(cpu, 6, 2, 4, address, old, source);
		else
			prepare_modify(cpu, 6, 3, 4, address, source);
		test::require(mapping.execute(atomic_test::pc, instruction, cpu) ==
			gadgets::Event::system_call);
		std::array<std::uint64_t, 2> actual{};
		std::memcpy(actual.data(), mapping.at(address), 16);
		test::require(actual == old && cpu.nzcv == 0);
	}

	const auto require_illegal = [&mapping](std::uint32_t instruction) {
		arm64::CpuState cpu{};
		test::require(mapping.execute(atomic_test::pc, instruction, cpu) ==
			gadgets::Event::illegal_instruction);
	};
	for (const auto instruction : {
		rcw_cas_pair(false, false, false, 6, 3, 4),
		rcw_cas_pair(false, false, false, 6, 2, 5),
		rcw_modify_pair(false, false, false, 1, 6, 31, 4),
		rcw_modify_pair(false, false, false, 1, 6, 3, 31),
		rcw_modify_pair(false, false, false, 1, 6, 3, 3)})
		require_illegal(instruction);
	for (unsigned operation = 4; operation < 8; ++operation)
		require_illegal(rcw_modify_pair(false, false, false,
			operation, 6, 3, 4));

	arm64::CpuState cpu{};
	prepare_cas(cpu, 6, 2, 4, address + 8, initial, operand);
	const auto before = cpu;
	test::require(mapping.execute(atomic_test::pc,
		rcw_cas_pair(false, true, true, 6, 2, 4), cpu) ==
		gadgets::Event::memory_fault);
	test::require(cpu.pc == atomic_test::pc &&
		cpu.registers == before.registers && cpu.nzcv == before.nzcv);
	cpu = {};
	prepare_modify(cpu, 6, 3, 4,
		mapping.guest() + test::StaticMapping::length, operand);
	const auto unmapped_before = cpu;
	test::require(mapping.execute(atomic_test::pc,
		rcw_modify_pair(true, true, true, 3, 6, 3, 4), cpu) ==
		gadgets::Event::memory_fault);
	test::require(cpu.pc == atomic_test::pc &&
		cpu.registers == unmapped_before.registers &&
		cpu.nzcv == unmapped_before.nzcv);

	arm64::CpuState warm{};
	warm.registers[6] = address;
	warm.registers[3] = initial[0];
	test::require(mapping.execute(atomic_test::pc,
		test::unsigned_immediate(3, 1, 0, 6, 8), warm) ==
		gadgets::Event::system_call);
	test::require(mapping.execute(atomic_test::pc,
		test::unsigned_immediate(3, 0, 0, 6, 3), warm) ==
		gadgets::Event::system_call);
	for (const bool compare_failure : {false, true}) {
		auto old = initial;
		auto compare = old;
		auto replacement = operand;
		if (compare_failure)
			compare[1] ^= 2U;
		else {
			old[0] |= 1U;
			compare = old;
			replacement = {old[0] & ~1U, old[1]};
		}
		std::memcpy(mapping.at(address), old.data(), 16);
		cpu = {};
		cpu.registers[6] = address;
		set_pair_registers(cpu, 2, compare);
		set_pair_registers(cpu, 4, replacement);
		cpu.registers[8] = ~old[0];
		const std::array stream{
			atomic_test::scalar_exclusive(3, true, 6, 7),
			rcw_cas_pair(!compare_failure, false, false, 6, 2, 4),
			atomic_test::scalar_exclusive(3, false, 6, 8, 9),
			0xd400'0001U};
		test::require(mapping.execute(atomic_test::pc,
			std::span<const std::uint32_t>{stream}, cpu) ==
			gadgets::Event::system_call);
		test::require(cpu.registers[9] == 1);
	}
}

void production_gadget_specialization()
{
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
	return;
#else
	std::set<std::uintptr_t> entries;
	for (const bool soft : {false, true})
		for (const bool acquire : {false, true})
			for (const bool release : {false, true})
				for (unsigned mode = 0; mode < 4; ++mode) {
					const auto built = gadgets::Builder{}.build(atomic_test::pc,
						std::array{rcw_cas_pair(soft, acquire, release, 6,
							(mode & 1U) != 0 ? 30U : 2U,
							(mode & 2U) != 0 ? 30U : 4U)});
					test::require(built.complete);
					entries.insert(built.program.words().front());
				}
	for (unsigned operation = 1; operation < 4; ++operation)
		for (const bool soft : {false, true})
			for (const bool acquire : {false, true})
				for (const bool release : {false, true}) {
					const auto built = gadgets::Builder{}.build(atomic_test::pc,
						std::array{rcw_modify_pair(soft, acquire, release,
							operation, 6, 3, 4)});
					test::require(built.complete);
					entries.insert(built.program.words().front());
				}
	test::require(entries.size() == 56);
#endif
}

} // namespace

int main()
{
	exhaustive_interpreter_matrix();
	interpreter_outcomes_faults_and_constraints();
	interpreter_overlaps_reservations_and_atomicity();
	exhaustive_gadget_matrix_and_edges();
	production_gadget_specialization();
}
