// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_rcpc_test_support.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <span>
#include <thread>

namespace rcpc = arm64_rcpc_test;
namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

enum class Kind : unsigned { store, ldap, ldapp };

constexpr std::array kinds{Kind::store, Kind::ldap, Kind::ldapp};
constexpr std::array first_pair{
	std::uint64_t{0x1021'3243'5465'7687},
	std::uint64_t{0x98a9'bacb'dced'fe0f}};
constexpr std::array second_pair{
	std::uint64_t{0xf1e2'd3c4'b5a6'9788},
	std::uint64_t{0x0f1e'2d3c'4b5a'6978}};

[[nodiscard]] constexpr bool is_load(Kind kind)
{
	return kind != Kind::store;
}

[[nodiscard]] constexpr std::uint32_t consistent_pair(Kind kind,
	unsigned rn, unsigned rt, unsigned rt2)
{
	const auto fixed = kind == Kind::store ? 0xd900'5800U :
		kind == Kind::ldap ? 0xd940'5800U : 0xd940'7800U;
	return fixed | ((rt2 & 31U) << 16) | ((rn & 31U) << 5) |
		(rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t ordered_allocation(unsigned size,
	bool load, unsigned opc2, unsigned rn = 2, unsigned rt = 3,
	unsigned rt2 = 4)
{
	return 0x1900'0800U | ((size & 3U) << 30) |
		(static_cast<unsigned>(load) << 22) | ((rt2 & 31U) << 16) |
		((opc2 & 15U) << 12) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr bool legal_ordered_encoding(std::uint32_t instruction)
{
	switch (instruction & 0xffe0'fc00U) {
	case 0x9900'0800U:
	case 0xd900'0800U:
	case 0x9900'1800U:
	case 0xd900'1800U:
	case 0x9940'0800U:
	case 0xd940'0800U:
	case 0x9940'1800U:
	case 0xd940'1800U:
	case 0xd900'5800U:
	case 0xd940'5800U:
	case 0xd940'7800U:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] constexpr std::uint32_t scalar_exclusive(bool load,
	unsigned rn, unsigned rt, unsigned status = 31)
{
	return 0xc800'0000U | (static_cast<unsigned>(load) << 22) |
		((status & 31U) << 16) | (31U << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

void put_pair(rcpc::Memory &memory, std::uint64_t address,
	const std::array<std::uint64_t, 2> &pair)
{
	memory.put(address, pair[0], 8);
	memory.put(address + 8, pair[1], 8);
}

[[nodiscard]] std::array<std::uint64_t, 2> get_pair(
	const rcpc::Memory &memory, std::uint64_t address)
{
	return {memory.get(address, 8), memory.get(address + 8, 8)};
}

void put_mapping_pair(test::StaticMapping &mapping, std::uint64_t address,
	const std::array<std::uint64_t, 2> &pair)
{
	std::memcpy(mapping.at(address), pair.data(), 16);
}

[[nodiscard]] std::array<std::uint64_t, 2> get_mapping_pair(
	test::StaticMapping &mapping, std::uint64_t address)
{
	std::array<std::uint64_t, 2> pair{};
	std::memcpy(pair.data(), mapping.at(address), 16);
	return pair;
}

void exhaustive_interpreter_matrix()
{
	for (const bool direct : {false, true}) {
		for (const auto kind : kinds) {
			for (const auto granule_offset : {0U, 8U}) {
				for (unsigned register_mode = 0; register_mode < 4;
				     ++register_mode) {
					for (const bool sp_base : {false, true}) {
						const unsigned first =
							(register_mode & 2U) ? 31U : 3U;
						const unsigned second =
							(register_mode & 1U) ? 31U : 4U;
						const auto address = rcpc::base + granule_offset;
						rcpc::Memory memory;
						memory.direct = direct;
						put_pair(memory, address, first_pair);
						arm64::CpuState cpu{};
						test::fill_registers(cpu);
						cpu.pc = rcpc::pc;
						if (sp_base)
							cpu.sp = address;
						else
							cpu.registers[2] = address;
						cpu.registers[3] = second_pair[0];
						cpu.registers[4] = second_pair[1];
						const auto before = cpu;
						test::require(arm64::Dispatcher{}.execute(cpu, memory,
							consistent_pair(kind, sp_base ? 31U : 2U,
								first, second)).reason ==
							arm64::StopReason::continued);
						test::require(cpu.pc == rcpc::pc + 4);
						if (is_load(kind)) {
							if (first != 31U)
								test::require(cpu.registers[first] ==
									first_pair[0]);
							else
								test::require(cpu.registers[3] ==
									before.registers[3]);
							if (second != 31U)
								test::require(cpu.registers[second] ==
									first_pair[1]);
							else
								test::require(cpu.registers[4] ==
									before.registers[4]);
						} else {
							const std::array expected{
								first == 31U ? 0U : second_pair[0],
								second == 31U ? 0U : second_pair[1]};
							test::require(get_pair(memory, address) == expected);
						}
						test::require((sp_base ? cpu.sp : cpu.registers[2]) ==
							address);
						test::require(memory.atomic_pointers == 1);
						if (direct)
							test::require(memory.reads == 0 && memory.writes == 0);
						else if (is_load(kind))
							test::require(memory.reads == 1 && memory.writes == 0);
						else
							test::require(memory.reads == 0 && memory.writes == 1);
					}
				}
			}
		}
	}
}

void interpreter_invalid_fault_and_reservation_cases()
{
	for (unsigned size = 0; size < 4; ++size) {
		for (const bool load : {false, true}) {
			for (unsigned opc2 = 0; opc2 < 16; ++opc2) {
				const auto instruction =
					ordered_allocation(size, load, opc2);
				if (legal_ordered_encoding(instruction))
					continue;
				rcpc::Memory memory;
				arm64::CpuState cpu{};
				cpu.pc = rcpc::pc;
				cpu.registers[2] = rcpc::base;
				const auto before = cpu;
				test::require(arm64::Dispatcher{}.execute(cpu, memory,
					instruction).reason == arm64::StopReason::illegal_instruction);
				test::require(cpu.pc == before.pc &&
					cpu.registers == before.registers &&
					memory.atomic_pointers == 0 && memory.reads == 0 &&
					memory.writes == 0);
			}
		}
	}

	for (const auto kind : kinds) {
		rcpc::Memory memory;
		arm64::CpuState cpu{};
		cpu.pc = rcpc::pc;
		cpu.registers[2] = rcpc::base + 1;
		cpu.registers[3] = second_pair[0];
		cpu.registers[4] = second_pair[1];
		const auto before = cpu;
		const auto result = arm64::Dispatcher{}.execute(cpu, memory,
			consistent_pair(kind, 2, 3, 4));
		test::require(result.reason == arm64::StopReason::memory_fault &&
			result.address == rcpc::base + 1 &&
			result.write == !is_load(kind));
		test::require(cpu.pc == before.pc && cpu.registers == before.registers &&
			memory.atomic_pointers == 0);

		memory.fault = true;
		cpu = before;
		cpu.registers[2] = rcpc::base;
		const auto fault = arm64::Dispatcher{}.execute(cpu, memory,
			consistent_pair(kind, 2, 3, 4));
		test::require(fault.reason == arm64::StopReason::memory_fault &&
			fault.write == !is_load(kind) && cpu.pc == before.pc);
	}

	for (const auto kind : {Kind::ldap, Kind::ldapp}) {
		for (const unsigned duplicate : {0U, 30U}) {
			rcpc::Memory memory;
			arm64::CpuState cpu{};
			cpu.pc = rcpc::pc;
			cpu.registers[2] = rcpc::base;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				consistent_pair(kind, 2, duplicate, duplicate)).reason ==
				arm64::StopReason::illegal_instruction);
			test::require(memory.atomic_pointers == 0);
		}
	}

	for (const bool direct : {false, true}) {
		rcpc::Memory memory;
		memory.direct = direct;
		put_pair(memory, rcpc::base, first_pair);
		arm64::CpuState cpu{};
		cpu.pc = rcpc::pc;
		cpu.registers[2] = rcpc::base;
		cpu.registers[3] = first_pair[0];
		cpu.registers[4] = first_pair[1];
		cpu.registers[6] = second_pair[0];
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			scalar_exclusive(true, 2, 5)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			consistent_pair(Kind::store, 2, 3, 4)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			scalar_exclusive(false, 2, 6, 7)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[7] == 1);
	}
}

void exhaustive_gadget_matrix_and_edges()
{
	test::StaticMapping mapping{0x4a0'0000, 229,
		"/private/tmp/uml-base-consistent-pair.XXXXXX"};
	const auto aligned = mapping.guest() + 4 * test::StaticMapping::page_size +
		512;
	for (const auto kind : kinds) {
		for (const auto granule_offset : {0U, 8U}) {
			for (unsigned register_mode = 0; register_mode < 4;
			     ++register_mode) {
				for (const bool sp_base : {false, true}) {
					const unsigned first =
						(register_mode & 2U) ? 31U : 3U;
					const unsigned second =
						(register_mode & 1U) ? 31U : 4U;
					const auto address = aligned + granule_offset;
					put_mapping_pair(mapping, address, first_pair);
					arm64::CpuState cpu{};
					test::fill_registers(cpu);
					if (sp_base)
						cpu.sp = address;
					else
						cpu.registers[2] = address;
					cpu.registers[3] = second_pair[0];
					cpu.registers[4] = second_pair[1];
					const auto before = cpu;
					test::require(mapping.execute(rcpc::pc,
						consistent_pair(kind, sp_base ? 31U : 2U, first,
							second), cpu) == gadgets::Event::system_call);
					if (is_load(kind)) {
						if (first != 31U)
							test::require(cpu.registers[first] == first_pair[0]);
						else
							test::require(cpu.registers[3] == before.registers[3]);
						if (second != 31U)
							test::require(cpu.registers[second] == first_pair[1]);
						else
							test::require(cpu.registers[4] == before.registers[4]);
					} else {
						const std::array expected{
							first == 31U ? 0U : second_pair[0],
							second == 31U ? 0U : second_pair[1]};
						test::require(get_mapping_pair(mapping, address) == expected);
					}
					test::require((sp_base ? cpu.sp : cpu.registers[2]) ==
						address);
				}
			}
		}
	}

	for (unsigned size = 0; size < 4; ++size) {
		for (const bool load : {false, true}) {
			for (unsigned opc2 = 0; opc2 < 16; ++opc2) {
				const auto instruction =
					ordered_allocation(size, load, opc2);
				if (legal_ordered_encoding(instruction))
					continue;
				arm64::CpuState cpu{};
				cpu.registers[2] = aligned;
				test::require(mapping.execute(rcpc::pc, instruction, cpu) ==
					gadgets::Event::illegal_instruction);
			}
		}
	}

	for (const auto kind : {Kind::ldap, Kind::ldapp}) {
		for (const unsigned duplicate : {0U, 30U}) {
			arm64::CpuState cpu{};
			cpu.registers[2] = aligned;
			test::require(mapping.execute(rcpc::pc,
				consistent_pair(kind, 2, duplicate, duplicate), cpu) ==
				gadgets::Event::illegal_instruction);
		}
	}

	for (const auto kind : kinds) {
		arm64::CpuState cpu{};
		cpu.registers[2] = aligned + 1;
		cpu.registers[3] = second_pair[0];
		cpu.registers[4] = second_pair[1];
		const auto before = cpu.registers;
		test::require(mapping.execute(rcpc::pc,
			consistent_pair(kind, 2, 3, 4), cpu) ==
			gadgets::Event::memory_fault);
		test::require(cpu.pc == rcpc::pc && cpu.registers == before);

		cpu = {};
		cpu.registers[2] = mapping.guest() + test::StaticMapping::length;
		test::require(mapping.execute(rcpc::pc,
			consistent_pair(kind, 2, 3, 4), cpu) ==
			gadgets::Event::memory_fault);
	}

	/* At this element-aligned page boundary only the first doubleword is
	 * mapped. Both pages are translated before the split fallback performs a
	 * load or store, so a fault cannot partially update memory or registers. */
	const auto boundary = mapping.guest() + test::StaticMapping::length - 8;
	std::memcpy(mapping.at(boundary), &first_pair[0], 8);
	for (const auto kind : kinds) {
		arm64::CpuState cpu{};
		cpu.registers[2] = boundary;
		cpu.registers[3] = second_pair[0];
		cpu.registers[4] = second_pair[1];
		const auto before = cpu.registers;
		test::require(mapping.execute(rcpc::pc,
			consistent_pair(kind, 2, 3, 4), cpu) ==
			gadgets::Event::memory_fault);
		test::require(cpu.pc == rcpc::pc && cpu.registers == before);
		std::uint64_t actual{};
		std::memcpy(&actual, mapping.at(boundary), 8);
		test::require(actual == first_pair[0]);
	}

	put_mapping_pair(mapping, aligned, first_pair);
	arm64::CpuState cpu{};
	cpu.registers[2] = aligned;
	cpu.registers[3] = first_pair[0];
	cpu.registers[4] = first_pair[1];
	cpu.registers[6] = second_pair[0];
	const std::array stream{
		scalar_exclusive(true, 2, 5),
		consistent_pair(Kind::store, 2, 3, 4),
		scalar_exclusive(false, 2, 6, 7),
		0xd400'0001U};
	test::require(mapping.execute(rcpc::pc,
		std::span<const std::uint32_t>{stream}, cpu) ==
		gadgets::Event::system_call);
	test::require(cpu.registers[7] == 1);
}

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

[[nodiscard]] std::array<std::uint64_t, 2> native_load_pair(
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

[[nodiscard]] bool is_complete_pair(
	const std::array<std::uint64_t, 2> &pair)
{
	return pair == first_pair || pair == second_pair;
}

void gadget_single_copy_atomicity()
{
	test::StaticMapping mapping{0x4c0'0000, 233,
		"/private/tmp/uml-base-consistent-pair-atomic.XXXXXX"};
	const auto address = mapping.guest() +
		4 * test::StaticMapping::page_size + 512;
	auto *host = reinterpret_cast<std::uint64_t *>(mapping.at(address));
	put_mapping_pair(mapping, address, first_pair);

	/* Atomic native stores race each LSCP load. A torn pair would combine one
	 * doubleword from each generation and is therefore immediately visible. */
	for (const auto kind : {Kind::ldap, Kind::ldapp}) {
		std::atomic<bool> running{true};
		std::atomic<bool> ready{false};
		std::thread writer{[&] {
			ready.store(true, std::memory_order_release);
			while (running.load(std::memory_order_acquire)) {
				native_store_pair(host, second_pair);
				native_store_pair(host, first_pair);
			}
		}};
		while (!ready.load(std::memory_order_acquire))
			std::this_thread::yield();
		for (unsigned iteration = 0; iteration < 5000; ++iteration) {
			arm64::CpuState cpu{};
			cpu.registers[2] = address;
			test::require(mapping.execute(rcpc::pc,
				consistent_pair(kind, 2, 3, 4), cpu) ==
				gadgets::Event::system_call);
			test::require(is_complete_pair(
				{cpu.registers[3], cpu.registers[4]}));
		}
		running.store(false, std::memory_order_release);
		writer.join();
	}

	/* Conversely, repeatedly sample alternating STLP gadget stores with a
	 * native atomic pair load. No observer may see a torn generation. */
	std::atomic<bool> running{true};
	std::atomic<bool> ready{false};
	std::atomic<bool> torn{false};
	std::atomic<std::uint64_t> torn_first{0};
	std::atomic<std::uint64_t> torn_second{0};
	std::atomic<unsigned> observations{0};
	std::thread reader{[&] {
		ready.store(true, std::memory_order_release);
		while (running.load(std::memory_order_acquire)) {
			const auto pair = native_load_pair(host);
			if (!is_complete_pair(pair)) {
				torn_first.store(pair[0], std::memory_order_relaxed);
				torn_second.store(pair[1], std::memory_order_relaxed);
				torn.store(true, std::memory_order_relaxed);
			}
			observations.fetch_add(1, std::memory_order_relaxed);
		}
	}};
	while (!ready.load(std::memory_order_acquire))
		std::this_thread::yield();
	for (unsigned iteration = 0; iteration < 5000; ++iteration) {
		const auto &pair = iteration & 1U ? first_pair : second_pair;
		arm64::CpuState cpu{};
		cpu.registers[2] = address;
		cpu.registers[3] = pair[0];
		cpu.registers[4] = pair[1];
		test::require(mapping.execute(rcpc::pc,
			consistent_pair(Kind::store, 2, 3, 4), cpu) ==
			gadgets::Event::system_call);
	}
	running.store(false, std::memory_order_release);
	reader.join();
	test::require(observations.load(std::memory_order_relaxed) != 0);
	if (torn.load(std::memory_order_relaxed))
		std::fprintf(stderr, "torn pair: %016llx %016llx\n",
			static_cast<unsigned long long>(
				torn_first.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(
				torn_second.load(std::memory_order_relaxed)));
	test::require(!torn.load(std::memory_order_relaxed));
}

} // namespace

int main()
{
	exhaustive_interpreter_matrix();
	interpreter_invalid_fault_and_reservation_cases();
	exhaustive_gadget_matrix_and_edges();
	gadget_single_copy_atomicity();
}
