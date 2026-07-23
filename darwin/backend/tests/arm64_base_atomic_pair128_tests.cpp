// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_test_support.hpp"

#include <atomic>
#include <array>
#include <cstring>
#include <thread>

namespace atomic_test = arm64_atomic_test;
namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::array operations{1U, 3U, 8U};
constexpr std::array initial{
	0xf0f1'f2f3'f4f5'f6f7ULL, 0x0f1e'2d3c'4b5a'6978ULL};
constexpr std::array operand{
	0x3300'5500'7700'9900ULL, 0xcc00'aa00'8800'6600ULL};

[[nodiscard]] constexpr std::uint32_t pair128_raw(bool acquire, bool release,
	bool o3, unsigned opcode, unsigned rn, unsigned rt, unsigned rt2)
{
	return 0x1920'0000U | (static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) | ((rt2 & 31U) << 16) |
		(static_cast<unsigned>(o3) << 15) | ((opcode & 7U) << 12) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t pair128(bool acquire, bool release,
	unsigned operation, unsigned rn = 2, unsigned rt = 3,
	unsigned rt2 = 4)
{
	return pair128_raw(acquire, release, operation == 8U,
		operation == 8U ? 0U : operation, rn, rt, rt2);
}

[[nodiscard]] constexpr std::array<std::uint64_t, 2> expected(
	unsigned operation, const std::array<std::uint64_t, 2> &observed,
	const std::array<std::uint64_t, 2> &source)
{
	return {atomic_test::expected(operation, observed[0], source[0], 8),
		atomic_test::expected(operation, observed[1], source[1], 8)};
}

void exhaustive_interpreter_variants()
{
	for (const bool direct : {false, true}) {
		for (const auto operation : operations) {
			for (const bool acquire : {false, true}) {
				for (const bool release : {false, true}) {
					atomic_test::AtomicMemory memory;
					memory.direct = direct;
					memory.put_pair(atomic_test::interpreter_base, initial);
					arm64::CpuState cpu{};
					cpu.pc = atomic_test::pc;
					cpu.registers[2] = atomic_test::interpreter_base;
					cpu.registers[3] = operand[0];
					cpu.registers[4] = operand[1];
					test::require(arm64::Dispatcher{}.execute(cpu, memory,
						pair128(acquire, release, operation)).reason ==
						arm64::StopReason::continued);
					test::require(cpu.registers[3] == initial[0] &&
						cpu.registers[4] == initial[1] &&
						cpu.pc == atomic_test::pc + 4);
					test::require(memory.get_pair(
						atomic_test::interpreter_base) ==
						expected(operation, initial, operand));
					if (direct)
						test::require(memory.atomic_pointers != 0 &&
							memory.writes == 0);
					else
						test::require(memory.reads == 1 && memory.writes == 1);
				}
			}
		}
	}
}

void interpreter_edges_faults_and_constraints()
{
	for (const auto operation : operations) {
		atomic_test::AtomicMemory memory;
		memory.put_pair(atomic_test::interpreter_base, initial);
		arm64::CpuState cpu{};
		cpu.pc = atomic_test::pc;
		cpu.sp = atomic_test::interpreter_base;
		cpu.registers[3] = operand[0];
		cpu.registers[4] = operand[1];
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			pair128(true, true, operation, 31)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.sp == atomic_test::interpreter_base &&
			cpu.registers[3] == initial[0] && cpu.registers[4] == initial[1]);

		/* Rn==Rt consumes the original base as operand before returning data. */
		memory = {};
		memory.put_pair(atomic_test::interpreter_base, initial);
		cpu = {};
		cpu.pc = atomic_test::pc;
		cpu.registers[3] = atomic_test::interpreter_base;
		cpu.registers[4] = operand[1];
		const std::array overlap_operand{
			atomic_test::interpreter_base, operand[1]};
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			pair128(false, false, operation, 3)).reason ==
			arm64::StopReason::continued);
		test::require(memory.get_pair(atomic_test::interpreter_base) ==
			expected(operation, initial, overlap_operand));
		test::require(cpu.registers[3] == initial[0]);

		/* Rn==Rt2 has the same read-before-write requirement. */
		memory = {};
		memory.put_pair(atomic_test::interpreter_base, initial);
		cpu = {};
		cpu.pc = atomic_test::pc;
		cpu.registers[3] = operand[0];
		cpu.registers[4] = atomic_test::interpreter_base;
		const std::array second_overlap_operand{
			operand[0], atomic_test::interpreter_base};
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			pair128(false, false, operation, 4)).reason ==
			arm64::StopReason::continued);
		test::require(memory.get_pair(atomic_test::interpreter_base) ==
			expected(operation, initial, second_overlap_operand));
		test::require(cpu.registers[4] == initial[1]);
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
	for (const bool o3 : {false, true})
		for (unsigned opcode = 0; opcode < 8; ++opcode)
			/* o3=1 opcodes 1..3 are the FEAT_D128/FEAT_THE RCW
			 * pair allocation, not reserved LSE128 encodings.
			 */
			if ((!o3 && opcode != 1U && opcode != 3U) ||
			    (o3 && opcode > 3U))
				require_illegal(pair128_raw(false, false, o3,
					opcode, 2, 3, 4));
	for (const auto instruction : {
		pair128(false, false, 1, 2, 31, 4),
		pair128(false, false, 1, 2, 3, 31),
		pair128(false, false, 1, 2, 3, 3)})
		require_illegal(instruction);

	atomic_test::AtomicMemory memory;
	arm64::CpuState cpu{};
	cpu.pc = atomic_test::pc;
	cpu.registers[2] = atomic_test::interpreter_base + 8;
	cpu.registers[3] = operand[0];
	cpu.registers[4] = operand[1];
	const auto before = cpu;
	const auto alignment = arm64::Dispatcher{}.execute(cpu, memory,
		pair128(true, true, 1));
	test::require(alignment.reason == arm64::StopReason::memory_fault &&
		alignment.address == atomic_test::interpreter_base + 8 &&
		alignment.write);
	test::require(cpu.pc == before.pc && cpu.registers == before.registers &&
		memory.atomic_pointers == 0);

	memory = {};
	memory.fault = true;
	cpu = {};
	cpu.pc = atomic_test::pc;
	cpu.registers[2] = atomic_test::interpreter_base;
	cpu.registers[3] = operand[0];
	cpu.registers[4] = operand[1];
	const auto fault_before = cpu;
	const auto fault = arm64::Dispatcher{}.execute(cpu, memory,
		pair128(true, true, 3));
	test::require(fault.reason == arm64::StopReason::memory_fault &&
		fault.address == atomic_test::interpreter_base && fault.write);
	test::require(cpu.pc == fault_before.pc &&
		cpu.registers == fault_before.registers);
}

void interpreter_reservation_invalidation()
{
	for (const bool direct : {false, true}) {
		atomic_test::AtomicMemory memory;
		memory.direct = direct;
		memory.put_pair(atomic_test::interpreter_base, initial);
		arm64::CpuState cpu{};
		cpu.pc = atomic_test::pc;
		cpu.registers[2] = atomic_test::interpreter_base;
		cpu.registers[3] = initial[0];
		cpu.registers[4] = initial[1];
		cpu.registers[6] = ~initial[0];
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			atomic_test::scalar_exclusive(3, true, 2, 5)).reason ==
			arm64::StopReason::continued);
		/* Even an equal-value SWPP is a write and conflicts with the
		 * reservation covering the first doubleword. */
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			pair128(false, false, 8)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			atomic_test::scalar_exclusive(3, false, 2, 6, 7)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[7] == 1 &&
			memory.get_pair(atomic_test::interpreter_base) == initial);
	}
}

[[nodiscard]] bool complete_pair(
	const std::array<std::uint64_t, 2> &pair)
{
	return pair == initial || pair == operand;
}

void interpreter_single_copy_atomicity()
{
	/* A native atomic writer races an interpreter LDCLRP with a zero mask.
	 * The RMW is value-preserving, so its returned old value must be one whole
	 * generation and never a pair assembled by copy_n.
	 */
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
		cpu.pc = atomic_test::pc;
		cpu.registers[2] = atomic_test::interpreter_base;
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			pair128(true, true, 1)).reason ==
			arm64::StopReason::continued);
		test::require(complete_pair(
			{cpu.registers[3], cpu.registers[4]}));
	}
	running.store(false, std::memory_order_release);
	writer.join();

	/* Conversely, sample alternating interpreter SWPP stores with an atomic
	 * native pair load. Every visible store must be a complete generation.
	 */
	memory.put_pair(atomic_test::interpreter_base, initial);
	running.store(true, std::memory_order_relaxed);
	ready.store(false, std::memory_order_relaxed);
	std::atomic<bool> torn{false};
	std::thread reader{[&] {
		ready.store(true, std::memory_order_release);
		while (running.load(std::memory_order_acquire))
			if (!complete_pair(atomic_test::native_load_pair(host)))
				torn.store(true, std::memory_order_relaxed);
	}};
	while (!ready.load(std::memory_order_acquire))
		std::this_thread::yield();
	for (unsigned iteration = 0; iteration < 5000; ++iteration) {
		const auto &pair = iteration & 1U ? initial : operand;
		arm64::CpuState cpu{};
		cpu.pc = atomic_test::pc;
		cpu.registers[2] = atomic_test::interpreter_base;
		cpu.registers[3] = pair[0];
		cpu.registers[4] = pair[1];
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			pair128(true, true, 8)).reason ==
			arm64::StopReason::continued);
	}
	running.store(false, std::memory_order_release);
	reader.join();
	test::require(!torn.load(std::memory_order_relaxed));
}

void exhaustive_gadget_variants_and_edges()
{
	test::StaticMapping mapping{0x420'0000, 183,
		"/private/tmp/uml-base-atomic-pair128.XXXXXX"};
	const auto address = mapping.guest() + 4 * test::StaticMapping::page_size;
	for (const auto operation : operations) {
		for (const bool acquire : {false, true}) {
			for (const bool release : {false, true}) {
				std::memcpy(mapping.at(address), initial.data(), 16);
				arm64::CpuState cpu{};
				cpu.registers[2] = address;
				cpu.registers[3] = operand[0];
				cpu.registers[4] = operand[1];
				test::require(mapping.execute(atomic_test::pc,
					pair128(acquire, release, operation), cpu) ==
					gadgets::Event::system_call);
				test::require(cpu.registers[3] == initial[0] &&
					cpu.registers[4] == initial[1]);
				std::array<std::uint64_t, 2> actual{};
				std::memcpy(actual.data(), mapping.at(address), 16);
				test::require(actual == expected(operation, initial, operand));
			}
		}

		std::memcpy(mapping.at(address), initial.data(), 16);
		arm64::CpuState cpu{};
		cpu.sp = address;
		cpu.registers[3] = operand[0];
		cpu.registers[4] = operand[1];
		test::require(mapping.execute(atomic_test::pc,
			pair128(true, true, operation, 31), cpu) ==
			gadgets::Event::system_call);
			test::require(cpu.sp == address);

		std::memcpy(mapping.at(address), initial.data(), 16);
		cpu = {};
		cpu.registers[3] = address;
		cpu.registers[4] = operand[1];
		const std::array first_overlap_operand{address, operand[1]};
		test::require(mapping.execute(atomic_test::pc,
			pair128(false, false, operation, 3), cpu) ==
			gadgets::Event::system_call);
		std::array<std::uint64_t, 2> overlap_actual{};
		std::memcpy(overlap_actual.data(), mapping.at(address), 16);
		test::require(overlap_actual ==
			expected(operation, initial, first_overlap_operand));
		test::require(cpu.registers[3] == initial[0]);

		std::memcpy(mapping.at(address), initial.data(), 16);
		cpu = {};
		cpu.registers[3] = operand[0];
		cpu.registers[4] = address;
		const std::array second_overlap_operand{operand[0], address};
		test::require(mapping.execute(atomic_test::pc,
			pair128(false, false, operation, 4), cpu) ==
			gadgets::Event::system_call);
		std::memcpy(overlap_actual.data(), mapping.at(address), 16);
		test::require(overlap_actual ==
			expected(operation, initial, second_overlap_operand));
		test::require(cpu.registers[4] == initial[1]);
	}

	const auto require_illegal = [&mapping](std::uint32_t instruction) {
		arm64::CpuState cpu{};
		test::require(mapping.execute(atomic_test::pc, instruction, cpu) ==
			gadgets::Event::illegal_instruction);
	};
	for (const bool o3 : {false, true})
		for (unsigned opcode = 0; opcode < 8; ++opcode)
			if ((!o3 && opcode != 1U && opcode != 3U) ||
			    (o3 && opcode > 3U))
				require_illegal(pair128_raw(false, false, o3,
					opcode, 2, 3, 4));
	for (const auto instruction : {
		pair128(false, false, 1, 2, 31, 4),
		pair128(false, false, 1, 2, 3, 31),
		pair128(false, false, 1, 2, 3, 3)})
		require_illegal(instruction);

	arm64::CpuState cpu{};
	cpu.registers[2] = address + 8;
	cpu.registers[3] = operand[0];
	cpu.registers[4] = operand[1];
	const auto before = cpu.registers;
	test::require(mapping.execute(atomic_test::pc,
		pair128(true, true, 8), cpu) == gadgets::Event::memory_fault);
	test::require(cpu.pc == atomic_test::pc && cpu.registers == before);

	cpu = {};
	cpu.registers[2] = mapping.guest() + test::StaticMapping::length;
	cpu.registers[3] = operand[0];
	cpu.registers[4] = operand[1];
	const auto unmapped_before = cpu.registers;
	test::require(mapping.execute(atomic_test::pc,
		pair128(true, true, 3), cpu) == gadgets::Event::memory_fault);
	test::require(cpu.pc == atomic_test::pc &&
		cpu.registers == unmapped_before);

	/* Warm both TLB permissions before establishing the host monitor, so the
	 * pair atomic itself is what invalidates the reservation. */
	arm64::CpuState warm{};
	warm.registers[2] = address;
	warm.registers[3] = initial[0];
	test::require(mapping.execute(atomic_test::pc,
		test::unsigned_immediate(3, 1, 0, 2, 8), warm) ==
		gadgets::Event::system_call);
	test::require(mapping.execute(atomic_test::pc,
		test::unsigned_immediate(3, 0, 0, 2, 3), warm) ==
		gadgets::Event::system_call);
	std::memcpy(mapping.at(address), initial.data(), 16);
	cpu = {};
	cpu.registers[2] = address;
	cpu.registers[3] = initial[0];
	cpu.registers[4] = initial[1];
	cpu.registers[6] = ~initial[0];
	const std::array invalidate_stream{
		atomic_test::scalar_exclusive(3, true, 2, 5),
		pair128(false, false, 8),
		atomic_test::scalar_exclusive(3, false, 2, 6, 7),
		0xd400'0001U};
	test::require(mapping.execute(atomic_test::pc,
		std::span<const std::uint32_t>{invalidate_stream}, cpu) ==
		gadgets::Event::system_call);
	test::require(cpu.registers[7] == 1);
	std::array<std::uint64_t, 2> after_invalidation{};
	std::memcpy(after_invalidation.data(), mapping.at(address), 16);
	test::require(after_invalidation == initial);
}

} // namespace

int main()
{
	exhaustive_interpreter_variants();
	interpreter_edges_faults_and_constraints();
	interpreter_reservation_invalidation();
	interpreter_single_copy_atomicity();
	exhaustive_gadget_variants_and_edges();
}
