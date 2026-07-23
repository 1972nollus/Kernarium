// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_load_store_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace arm64_atomic_test {

namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

inline constexpr std::uint64_t pc = 0x3b0'0000;
inline constexpr std::uint64_t interpreter_base = 0xe1'0080;

[[nodiscard]] inline constexpr std::uint32_t atomic_raw(unsigned size_code,
	bool acquire, bool release, bool o3, unsigned opcode, unsigned rn,
	unsigned rs, unsigned rt)
{
	return 0x3820'0000U | ((size_code & 3U) << 30) |
		(static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) | ((rs & 31U) << 16) |
		(static_cast<unsigned>(o3) << 15) | ((opcode & 7U) << 12) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr std::uint32_t atomic(unsigned size_code,
	bool acquire, bool release, unsigned operation, unsigned rn,
	unsigned rs, unsigned rt)
{
	return atomic_raw(size_code, acquire, release, operation == 8U,
		operation == 8U ? 0U : operation, rn, rs, rt);
}

[[nodiscard]] inline constexpr std::uint32_t unprivileged_atomic_raw(
	unsigned size_code, bool acquire, bool release, bool o3, unsigned opcode,
	unsigned rn, unsigned rs, unsigned rt)
{
	return 0x1920'0400U | ((size_code & 3U) << 30) |
		(static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) | ((rs & 31U) << 16) |
		(static_cast<unsigned>(o3) << 15) | ((opcode & 7U) << 12) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr std::uint32_t unprivileged_atomic(
	unsigned size_code, bool acquire, bool release, unsigned operation,
	unsigned rn, unsigned rs, unsigned rt)
{
	return unprivileged_atomic_raw(size_code, acquire, release,
		operation == 8U, operation == 8U ? 0U : operation, rn, rs, rt);
}

[[nodiscard]] inline constexpr std::uint32_t scalar_exclusive(
	unsigned size_code, bool load, unsigned rn, unsigned rt,
	unsigned status = 31)
{
	return 0x0800'0000U | ((size_code & 3U) << 30) |
		(static_cast<unsigned>(load) << 22) | ((status & 31U) << 16) |
		(31U << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr bool has_unprivileged_form(unsigned operation)
{
	return operation == 0U || operation == 1U || operation == 3U ||
		operation == 8U;
}

[[nodiscard]] inline constexpr std::uint64_t mask_for_size(std::size_t size)
{
	return size == 8 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << (size * 8)) - 1;
}

[[nodiscard]] inline constexpr std::int64_t signed_for_size(
	std::uint64_t value, std::size_t size)
{
	const auto bits = static_cast<unsigned>(size * 8);
	if (bits == 64)
		return static_cast<std::int64_t>(value);
	const auto shift = 64U - bits;
	return static_cast<std::int64_t>(value << shift) >> shift;
}

[[nodiscard]] inline constexpr std::uint64_t expected(unsigned operation,
	std::uint64_t observed, std::uint64_t operand, std::size_t size)
{
	const auto mask = mask_for_size(size);
	observed &= mask;
	operand &= mask;
	switch (operation) {
	case 0: return (observed + operand) & mask;
	case 1: return observed & ~operand & mask;
	case 2: return observed ^ operand;
	case 3: return observed | operand;
	case 4: return signed_for_size(observed, size) >=
			signed_for_size(operand, size) ? observed : operand;
	case 5: return signed_for_size(observed, size) <=
			signed_for_size(operand, size) ? observed : operand;
	case 6: return observed >= operand ? observed : operand;
	case 7: return observed <= operand ? observed : operand;
	case 8: return operand;
	default: return observed;
	}
}

[[nodiscard]] inline constexpr std::array<std::array<std::uint64_t, 2>, 2>
value_pairs(std::size_t size)
{
	const auto sign = std::uint64_t{1} << (size * 8 - 1);
	const auto negative = sign | 0x15U;
	const auto positive = sign - 0x22U;
	return {{{negative, positive}, {positive, negative}}};
}

class alignas(64) AtomicMemory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> output) override
	{
		++reads;
		if (fault || fault_read || !range_valid(address, output.size()))
			return false;
		std::copy_n(pointer(address), output.size(), output.begin());
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> input) override
	{
		++writes;
		if (fault || fault_write || !range_valid(address, input.size()))
			return false;
		std::copy(input.begin(), input.end(), pointer(address));
		return true;
	}

	[[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
		std::size_t size, arm64::MemoryAccess) override
	{
		++atomic_pointers;
		return direct && !fault && !fault_write &&
			range_valid(address, size) ?
			pointer(address) : nullptr;
	}

	void put(std::uint64_t address, std::uint64_t value, std::size_t size)
	{
		std::memcpy(pointer(address), &value, size);
	}

	[[nodiscard]] std::uint64_t get(std::uint64_t address,
		std::size_t size) const
	{
		std::uint64_t value{};
		std::memcpy(&value, pointer(address), size);
		return value;
	}

	void put_pair(std::uint64_t address,
		const std::array<std::uint64_t, 2> &values)
	{
		std::memcpy(pointer(address), values.data(), 16);
	}

	[[nodiscard]] std::array<std::uint64_t, 2> get_pair(
		std::uint64_t address) const
	{
		std::array<std::uint64_t, 2> values{};
		std::memcpy(values.data(), pointer(address), 16);
		return values;
	}

	[[nodiscard]] std::uint64_t *host_pair(std::uint64_t address)
	{
		return reinterpret_cast<std::uint64_t *>(pointer(address));
	}

	bool direct{true};
	bool fault{};
	bool fault_read{};
	bool fault_write{};
	unsigned reads{};
	unsigned writes{};
	unsigned atomic_pointers{};

private:
	[[nodiscard]] bool range_valid(std::uint64_t address,
		std::size_t size) const
	{
		return address >= interpreter_base - 64 &&
			address - (interpreter_base - 64) <= bytes.size() &&
			size <= bytes.size() -
				(address - (interpreter_base - 64));
	}

	[[nodiscard]] std::byte *pointer(std::uint64_t address)
	{
		return bytes.data() + (address - (interpreter_base - 64));
	}

	[[nodiscard]] const std::byte *pointer(std::uint64_t address) const
	{
		return bytes.data() + (address - (interpreter_base - 64));
	}

	alignas(64) std::array<std::byte, 256> bytes{};
};

inline void native_store_pair(std::uint64_t *pointer,
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

[[nodiscard]] inline std::array<std::uint64_t, 2> native_load_pair(
	const std::uint64_t *pointer)
{
	std::array<std::uint64_t, 2> pair{};
	__asm__ volatile(
		"ldxp %0, %1, [%2]\n"
		"clrex"
		: "=&r"(pair[0]), "=&r"(pair[1])
		: "r"(pointer)
		: "memory");
	return pair;
}

inline void run_interpreter_case(std::uint32_t instruction,
	unsigned operation, std::size_t size, bool direct,
	std::uint64_t observed, std::uint64_t operand)
{
	AtomicMemory memory;
	memory.direct = direct;
	memory.put(interpreter_base, observed, size);
	arm64::CpuState cpu{};
	test::fill_registers(cpu);
	cpu.pc = pc;
	cpu.registers[2] = interpreter_base;
	cpu.registers[3] = operand;
	const auto untouched = cpu.registers[5];
	const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
	test::require(result.reason == arm64::StopReason::continued);
	test::require(cpu.pc == pc + 4);
	test::require(cpu.registers[4] == (observed & mask_for_size(size)));
	test::require(cpu.registers[5] == untouched);
	test::require(memory.get(interpreter_base, size) ==
		expected(operation, observed, operand, size));
	if (direct)
		test::require(memory.atomic_pointers != 0 && memory.writes == 0);
	else
		test::require(memory.reads == 1 && memory.writes == 1);
}

inline void interpreter_matrix(std::span<const unsigned> operations)
{
	for (const bool direct : {false, true}) {
		for (const auto operation : operations) {
			for (unsigned size_code = 0; size_code < 4; ++size_code) {
				const auto size = std::size_t{1} << size_code;
				for (const bool acquire : {false, true}) {
					for (const bool release : {false, true}) {
						for (const auto values : value_pairs(size)) {
							run_interpreter_case(atomic(size_code,
								acquire, release, operation, 2, 3, 4),
								operation, size, direct, values[0],
								values[1]);
						}
					}
				}
			}
			if (!has_unprivileged_form(operation))
				continue;
			for (unsigned encoded_size = 0; encoded_size < 2;
			     ++encoded_size) {
				const auto size = std::size_t{4} << encoded_size;
				for (const bool acquire : {false, true}) {
					for (const bool release : {false, true}) {
						for (const auto values : value_pairs(size)) {
							run_interpreter_case(unprivileged_atomic(
								encoded_size, acquire, release,
								operation, 2, 3, 4), operation, size,
								direct, values[0], values[1]);
						}
					}
				}
			}
			if (has_unprivileged_form(operation)) {
				for (unsigned encoded_size = 0; encoded_size < 2;
				     ++encoded_size) {
					const auto size = std::size_t{4} << encoded_size;
					const auto observed = value_pairs(size)[0][0];
					const auto operand = value_pairs(size)[0][1];
					for (const bool release : {false, true}) {
						AtomicMemory memory;
						memory.direct = direct;
						memory.put(interpreter_base, observed, size);
						arm64::CpuState cpu{};
						cpu.pc = pc;
						cpu.registers[2] = interpreter_base;
						cpu.registers[3] = operand;
						test::require(arm64::Dispatcher{}.execute(
							cpu, memory, unprivileged_atomic(encoded_size,
								false, release, operation, 2, 3, 31)).reason ==
							arm64::StopReason::continued);
						test::require(memory.get(interpreter_base, size) ==
							expected(operation, observed, operand, size));
					}
				}
			}
		}
	}
}

inline void run_gadget_case(test::StaticMapping &mapping,
	std::uint64_t address, std::uint32_t instruction, unsigned operation,
	std::size_t size, std::uint64_t observed, std::uint64_t operand)
{
	std::memcpy(mapping.at(address), &observed, size);
	arm64::CpuState cpu{};
	test::fill_registers(cpu);
	cpu.registers[2] = address;
	cpu.registers[3] = operand;
	const auto untouched = cpu.registers[5];
	test::require(mapping.execute(pc, instruction, cpu) ==
		gadgets::Event::system_call);
	test::require(cpu.registers[4] == (observed & mask_for_size(size)));
	test::require(cpu.registers[5] == untouched);
	std::uint64_t actual{};
	std::memcpy(&actual, mapping.at(address), size);
	test::require(actual == expected(operation, observed, operand, size));
}

inline void gadget_matrix(std::span<const unsigned> operations,
	test::StaticMapping &mapping, std::uint64_t address)
{
	for (const auto operation : operations) {
		for (unsigned size_code = 0; size_code < 4; ++size_code) {
			const auto size = std::size_t{1} << size_code;
			for (const bool acquire : {false, true}) {
				for (const bool release : {false, true}) {
					for (const auto values : value_pairs(size)) {
						run_gadget_case(mapping, address, atomic(size_code,
							acquire, release, operation, 2, 3, 4),
							operation, size, values[0], values[1]);
					}
				}
			}
		}
		if (!has_unprivileged_form(operation))
			continue;
		for (unsigned encoded_size = 0; encoded_size < 2; ++encoded_size) {
			const auto size = std::size_t{4} << encoded_size;
			for (const bool acquire : {false, true}) {
				for (const bool release : {false, true}) {
					for (const auto values : value_pairs(size)) {
						run_gadget_case(mapping, address,
							unprivileged_atomic(encoded_size, acquire,
								release, operation, 2, 3, 4), operation,
								size, values[0], values[1]);
					}
				}
			}
		}
		if (has_unprivileged_form(operation)) {
			for (unsigned encoded_size = 0; encoded_size < 2;
			     ++encoded_size) {
				const auto size = std::size_t{4} << encoded_size;
				const auto observed = value_pairs(size)[0][0];
				const auto operand = value_pairs(size)[0][1];
				for (const bool release : {false, true}) {
					std::memcpy(mapping.at(address), &observed, size);
					arm64::CpuState cpu{};
					cpu.registers[2] = address;
					cpu.registers[3] = operand;
					test::require(mapping.execute(pc,
						unprivileged_atomic(encoded_size, false,
							release, operation, 2, 3, 31), cpu) ==
						gadgets::Event::system_call);
					std::uint64_t actual{};
					std::memcpy(&actual, mapping.at(address), size);
					test::require(actual ==
						expected(operation, observed, operand, size));
				}
			}
		}
	}
}

inline void interpreter_register_overlaps(std::span<const unsigned> operations)
{
	for (const bool direct : {false, true}) {
		for (const auto operation : operations) {
			constexpr std::size_t size = 8;
			const auto observed = value_pairs(size)[0][0];

			/* Rn==Rs: capture the address before the result is written. */
			AtomicMemory memory;
			memory.direct = direct;
			memory.put(interpreter_base, observed, size);
			arm64::CpuState cpu{};
			cpu.pc = pc;
			cpu.registers[2] = interpreter_base;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				atomic(3, false, false, operation, 2, 2, 4)).reason ==
				arm64::StopReason::continued);
			test::require(memory.get(interpreter_base, size) ==
				expected(operation, observed, interpreter_base, size));
			test::require(cpu.registers[4] == observed);

			/* Rs==Rt: preserve the operand until after the atomic update. */
			const auto operand = value_pairs(size)[0][1];
			memory = {};
			memory.direct = direct;
			memory.put(interpreter_base, observed, size);
			cpu = {};
			cpu.pc = pc;
			cpu.registers[2] = interpreter_base;
			cpu.registers[3] = operand;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				atomic(3, false, false, operation, 2, 3, 3)).reason ==
				arm64::StopReason::continued);
			test::require(memory.get(interpreter_base, size) ==
				expected(operation, observed, operand, size));
			test::require(cpu.registers[3] == observed);

			/* Rn==Rt: use the original base, then return the old value. */
			memory = {};
			memory.direct = direct;
			memory.put(interpreter_base, observed, size);
			cpu = {};
			cpu.pc = pc;
			cpu.registers[2] = interpreter_base;
			cpu.registers[3] = operand;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				atomic(3, false, false, operation, 2, 3, 2)).reason ==
				arm64::StopReason::continued);
			test::require(memory.get(interpreter_base, size) ==
				expected(operation, observed, operand, size));
			test::require(cpu.registers[2] == observed);
		}
	}
}

inline void gadget_register_overlaps(std::span<const unsigned> operations,
	test::StaticMapping &mapping, std::uint64_t address)
{
	for (const auto operation : operations) {
		constexpr std::size_t size = 8;
		const auto observed = value_pairs(size)[0][0];
		const auto operand = value_pairs(size)[0][1];

		std::memcpy(mapping.at(address), &observed, size);
		arm64::CpuState cpu{};
		cpu.registers[2] = address;
		test::require(mapping.execute(pc,
			atomic(3, false, false, operation, 2, 2, 4), cpu) ==
			gadgets::Event::system_call);
		std::uint64_t actual{};
		std::memcpy(&actual, mapping.at(address), size);
		test::require(actual == expected(operation, observed, address, size));
		test::require(cpu.registers[4] == observed);

		std::memcpy(mapping.at(address), &observed, size);
		cpu = {};
		cpu.registers[2] = address;
		cpu.registers[3] = operand;
		test::require(mapping.execute(pc,
			atomic(3, false, false, operation, 2, 3, 3), cpu) ==
			gadgets::Event::system_call);
		std::memcpy(&actual, mapping.at(address), size);
		test::require(actual == expected(operation, observed, operand, size));
		test::require(cpu.registers[3] == observed);

		std::memcpy(mapping.at(address), &observed, size);
		cpu = {};
		cpu.registers[2] = address;
		cpu.registers[3] = operand;
		test::require(mapping.execute(pc,
			atomic(3, false, false, operation, 2, 3, 2), cpu) ==
			gadgets::Event::system_call);
		std::memcpy(&actual, mapping.at(address), size);
		test::require(actual == expected(operation, observed, operand, size));
		test::require(cpu.registers[2] == observed);
	}
}

inline void interpreter_aliases_and_register_modes(
	std::span<const unsigned> operations)
{
	for (const bool direct : {false, true}) {
		for (const auto operation : operations) {
			for (unsigned size_code = 0; size_code < 4; ++size_code) {
				const auto size = std::size_t{1} << size_code;
				const auto observed = value_pairs(size)[0][0];
				const auto operand = value_pairs(size)[0][1];
				for (const bool release : {false, true}) {
					AtomicMemory memory;
					memory.direct = direct;
					memory.put(interpreter_base, observed, size);
					arm64::CpuState cpu{};
					cpu.pc = pc;
					cpu.registers[2] = interpreter_base;
					cpu.registers[3] = operand;
					test::require(arm64::Dispatcher{}.execute(cpu, memory,
						atomic(size_code, false, release, operation,
							2, 3, 31)).reason ==
						arm64::StopReason::continued);
					test::require(memory.get(interpreter_base, size) ==
						expected(operation, observed, operand, size));
				}

				/* Exercise source-ZR, discarded-result, and SP-base
				 * specialization together, including acquire suppression. */
				for (const bool discard : {false, true}) {
					AtomicMemory memory;
					memory.direct = direct;
					memory.put(interpreter_base, observed, size);
					arm64::CpuState cpu{};
					cpu.pc = pc;
					cpu.sp = interpreter_base;
					test::require(arm64::Dispatcher{}.execute(cpu, memory,
						atomic(size_code, true, true, operation, 31,
							31, discard ? 31U : 4U)).reason ==
						arm64::StopReason::continued);
					test::require(cpu.sp == interpreter_base);
					test::require(memory.get(interpreter_base, size) ==
						expected(operation, observed, 0, size));
					if (!discard)
						test::require(cpu.registers[4] ==
							(observed & mask_for_size(size)));
				}
			}
		}
	}
}

inline void gadget_aliases_and_register_modes(
	std::span<const unsigned> operations, test::StaticMapping &mapping,
	std::uint64_t address)
{
	for (const auto operation : operations) {
		for (unsigned size_code = 0; size_code < 4; ++size_code) {
			const auto size = std::size_t{1} << size_code;
			const auto observed = value_pairs(size)[0][0];
			const auto operand = value_pairs(size)[0][1];
			for (const bool release : {false, true}) {
				std::memcpy(mapping.at(address), &observed, size);
				arm64::CpuState cpu{};
				cpu.registers[2] = address;
				cpu.registers[3] = operand;
				test::require(mapping.execute(pc, atomic(size_code,
					false, release, operation, 2, 3, 31), cpu) ==
					gadgets::Event::system_call);
				std::uint64_t actual{};
				std::memcpy(&actual, mapping.at(address), size);
				test::require(actual ==
					expected(operation, observed, operand, size));
			}
			for (const bool discard : {false, true}) {
				std::memcpy(mapping.at(address), &observed, size);
				arm64::CpuState cpu{};
				cpu.sp = address;
				test::require(mapping.execute(pc, atomic(size_code, true,
					true, operation, 31, 31, discard ? 31U : 4U),
					cpu) == gadgets::Event::system_call);
				std::uint64_t actual{};
				std::memcpy(&actual, mapping.at(address), size);
				test::require(actual == expected(operation, observed, 0,
					size) && cpu.sp == address);
				if (!discard)
					test::require(cpu.registers[4] ==
						(observed & mask_for_size(size)));
			}
		}
	}
}

inline void interpreter_alignment_faults_and_reservations(
	std::span<const unsigned> operations)
{
	for (const auto operation : operations) {
		for (unsigned size_code = 1; size_code < 4; ++size_code) {
			AtomicMemory memory;
			arm64::CpuState cpu{};
			cpu.pc = pc;
			cpu.registers[2] = interpreter_base + 1;
			cpu.registers[3] = 7;
			const auto before = cpu;
			const auto result = arm64::Dispatcher{}.execute(cpu, memory,
				atomic(size_code, true, true, operation, 2, 3, 4));
			test::require(result.reason == arm64::StopReason::memory_fault &&
				result.address == interpreter_base + 1 && result.write);
			test::require(cpu.pc == pc && cpu.registers == before.registers &&
				memory.atomic_pointers == 0 && memory.reads == 0 &&
				memory.writes == 0);
		}

		AtomicMemory faulting;
		faulting.fault = true;
		arm64::CpuState fault_cpu{};
		fault_cpu.pc = pc;
		fault_cpu.registers[2] = interpreter_base;
		fault_cpu.registers[3] = 7;
		const auto fault_before = fault_cpu;
		const auto fault = arm64::Dispatcher{}.execute(fault_cpu, faulting,
			atomic(3, true, true, operation, 2, 3, 4));
		test::require(fault.reason == arm64::StopReason::memory_fault &&
			fault.address == interpreter_base && fault.write);
		test::require(fault_cpu.pc == pc &&
			fault_cpu.registers == fault_before.registers);

		/* Every RMW is a write and invalidates an existing reservation,
		 * including the equal-result cases in this matrix. */
		AtomicMemory memory;
		memory.put(interpreter_base, 0x1234, 8);
		arm64::CpuState cpu{};
		cpu.pc = pc;
		cpu.registers[2] = interpreter_base;
		cpu.registers[3] = operation == 8U ? 0x1234 : 0;
		cpu.registers[4] = 0x5678;
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			scalar_exclusive(3, true, 2, 5)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			atomic(3, false, false, operation, 2, 3, 31)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			scalar_exclusive(3, false, 2, 4, 6)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[6] == 1);
	}
}

inline void gadget_alignment_faults(std::span<const unsigned> operations,
	test::StaticMapping &mapping, std::uint64_t address)
{
	for (const auto operation : operations) {
		for (unsigned size_code = 1; size_code < 4; ++size_code) {
			arm64::CpuState cpu{};
			cpu.registers[2] = address + 1;
			cpu.registers[3] = 7;
			const auto before = cpu.registers;
			test::require(mapping.execute(pc, atomic(size_code, true, true,
				operation, 2, 3, 4), cpu) ==
				gadgets::Event::memory_fault);
			test::require(cpu.pc == pc && cpu.registers == before);
		}
		arm64::CpuState cpu{};
		cpu.registers[2] = mapping.guest() + test::StaticMapping::length;
		cpu.registers[3] = 7;
		const auto before = cpu.registers;
		test::require(mapping.execute(pc,
			atomic(3, true, true, operation, 2, 3, 4), cpu) ==
			gadgets::Event::memory_fault);
		test::require(cpu.pc == pc && cpu.registers == before);
	}
}

inline void invalid_encoding_tests(test::StaticMapping &mapping)
{
	const std::array invalid{
		atomic_raw(3, false, false, true, 4, 2, 3, 4),
		unprivileged_atomic_raw(0, false, false, false, 2, 2, 3, 4),
		unprivileged_atomic_raw(0, false, false, true, 1, 2, 3, 4),
		unprivileged_atomic_raw(2, false, false, false, 0, 2, 3, 4),
	};
	for (const auto instruction : invalid) {
		AtomicMemory memory;
		arm64::CpuState cpu{};
		cpu.pc = pc;
		const auto result = arm64::Dispatcher{}.execute(cpu, memory,
			instruction);
		test::require(result.reason == arm64::StopReason::illegal_instruction);
		test::require(cpu.pc == pc && memory.atomic_pointers == 0 &&
			memory.reads == 0 && memory.writes == 0);
		cpu = {};
		test::require(mapping.execute(pc, instruction, cpu) ==
			gadgets::Event::illegal_instruction);
	}

	/* The size-code-three o3:opc=1:001 LSE-shaped encoding is reserved. Pair
	 * FEAT_THE RCW instructions occupy the separate 0x1920xxxx allocation. */
	const auto reserved = atomic_raw(3, false, false, true, 1, 2, 3, 4);
	AtomicMemory memory;
	arm64::CpuState cpu{};
	cpu.pc = pc;
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		reserved).reason == arm64::StopReason::illegal_instruction);
	test::require(cpu.pc == pc && memory.atomic_pointers == 0 &&
		memory.reads == 0 && memory.writes == 0);
	cpu = {};
	test::require(mapping.execute(pc, reserved, cpu) ==
		gadgets::Event::illegal_instruction);
}

} // namespace arm64_atomic_test
