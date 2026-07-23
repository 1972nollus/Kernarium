// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_rcpc_test_support.hpp"

#include <array>
#include <cstring>

namespace rcpc = arm64_rcpc_test;
namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::array values{
	std::uint64_t{0xf1e2'd3c4'b5a6'9788},
	std::uint64_t{0x8071'6253'4435'2617}};
constexpr std::array replacements{
	std::uint64_t{0x1021'3243'5465'7687},
	std::uint64_t{0x899a'abbc'cdde'eff0}};

[[nodiscard]] constexpr std::uint32_t ordered_pair(unsigned size_code,
	bool load, bool writeback, unsigned rn, unsigned rt, unsigned rt2)
{
	return 0x9900'0800U | ((size_code & 3U) << 30) |
		(static_cast<unsigned>(load) << 22) | ((rt2 & 31U) << 16) |
		(static_cast<unsigned>(!writeback) << 12) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t scalar_exclusive(bool load,
	unsigned rn, unsigned rt, unsigned status = 31)
{
	return 0xc800'0000U | (static_cast<unsigned>(load) << 22) |
		((status & 31U) << 16) | (31U << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

void put_pair(rcpc::Memory &memory, std::uint64_t address,
	std::size_t element_size, const std::array<std::uint64_t, 2> &pair)
{
	memory.put(address, pair[0], element_size);
	memory.put(address + element_size, pair[1], element_size);
}

[[nodiscard]] std::array<std::uint64_t, 2> get_pair(
	const rcpc::Memory &memory, std::uint64_t address,
	std::size_t element_size)
{
	return {memory.get(address, element_size),
		memory.get(address + element_size, element_size)};
}

void put_mapping_pair(test::StaticMapping &mapping, std::uint64_t address,
	std::size_t element_size, const std::array<std::uint64_t, 2> &pair)
{
	std::memcpy(mapping.at(address), &pair[0], element_size);
	std::memcpy(mapping.at(address + element_size), &pair[1], element_size);
}

[[nodiscard]] std::array<std::uint64_t, 2> get_mapping_pair(
	test::StaticMapping &mapping, std::uint64_t address,
	std::size_t element_size)
{
	std::array<std::uint64_t, 2> pair{};
	std::memcpy(&pair[0], mapping.at(address), element_size);
	std::memcpy(&pair[1], mapping.at(address + element_size), element_size);
	return pair;
}

void exhaustive_interpreter_matrix()
{
	for (const bool direct : {false, true}) {
		for (unsigned size_code = 2; size_code < 4; ++size_code) {
			const auto element_size = std::size_t{1} << size_code;
			const auto total_size = element_size * 2;
			const auto mask = rcpc::mask(element_size);
			for (const bool load : {false, true}) {
				for (const bool writeback : {false, true}) {
					for (unsigned register_mode = 0; register_mode < 4;
					     ++register_mode) {
						for (const bool sp_base : {false, true}) {
							const unsigned first =
								(register_mode & 2U) ? 31U : 3U;
							const unsigned second =
								(register_mode & 1U) ? 31U : 4U;
							rcpc::Memory memory;
							memory.direct = direct;
							const auto address = load || !writeback ?
								rcpc::base : rcpc::base - total_size;
							put_pair(memory, address, element_size, values);
							arm64::CpuState cpu{};
							test::fill_registers(cpu);
							cpu.pc = rcpc::pc;
							if (sp_base)
								cpu.sp = rcpc::base;
							else
								cpu.registers[2] = rcpc::base;
							cpu.registers[3] = replacements[0];
							cpu.registers[4] = replacements[1];
							const auto before = cpu.registers;
							test::require(arm64::Dispatcher{}.execute(cpu,
								memory, ordered_pair(size_code, load,
									writeback, sp_base ? 31U : 2U, first,
									second)).reason ==
								arm64::StopReason::continued);
							test::require(cpu.pc == rcpc::pc + 4);
							if (load) {
								if (first != 31U)
									test::require(cpu.registers[first] ==
										(values[0] & mask));
								else
									test::require(cpu.registers[3] == before[3]);
								if (second != 31U)
									test::require(cpu.registers[second] ==
										(values[1] & mask));
								else
									test::require(cpu.registers[4] == before[4]);
							} else {
								const std::array expected{
									first == 31U ? 0U : replacements[0] & mask,
									second == 31U ? 0U : replacements[1] & mask};
								test::require(get_pair(memory, address,
									element_size) == expected);
							}
							const auto adjusted = load ? rcpc::base + total_size :
								rcpc::base - total_size;
							if (sp_base)
								test::require(cpu.sp == (writeback ? adjusted :
									rcpc::base));
							else
								test::require(cpu.registers[2] == (writeback ?
									adjusted : rcpc::base));
							test::require(memory.atomic_pointers == 1);
							if (direct)
								test::require(memory.reads == 0 &&
									memory.writes == 0);
							else if (load)
								test::require(memory.reads == 1 &&
									memory.writes == 0);
							else
								test::require(memory.reads == 0 &&
									memory.writes == 1);
						}
					}
				}
			}
		}
	}
}

void interpreter_overlaps_faults_and_reservations()
{
	for (unsigned size_code = 2; size_code < 4; ++size_code) {
		const auto element_size = std::size_t{1} << size_code;
		const auto total_size = element_size * 2;
		const auto mask = rcpc::mask(element_size);

		/* Post-index load/base overlap selects WBSUPPRESS. */
		for (const bool second_overlap : {false, true}) {
			rcpc::Memory memory;
			put_pair(memory, rcpc::base, element_size, values);
			arm64::CpuState cpu{};
			cpu.pc = rcpc::pc;
			cpu.registers[second_overlap ? 4U : 3U] = rcpc::base;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				ordered_pair(size_code, true, true,
					second_overlap ? 4U : 3U, 3, 4)).reason ==
				arm64::StopReason::continued);
			test::require(cpu.registers[3] == (values[0] & mask) &&
				cpu.registers[4] == (values[1] & mask));
		}

		/* Pre-index store/base overlap selects original source data. */
		for (const bool second_overlap : {false, true}) {
			rcpc::Memory memory;
			arm64::CpuState cpu{};
			cpu.pc = rcpc::pc;
			const unsigned base_register = second_overlap ? 4U : 3U;
			cpu.registers[base_register] = rcpc::base;
			cpu.registers[second_overlap ? 3U : 4U] = replacements[
				second_overlap ? 0U : 1U];
			const std::array expected{
				second_overlap ? replacements[0] & mask : rcpc::base & mask,
				second_overlap ? rcpc::base & mask : replacements[1] & mask};
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				ordered_pair(size_code, false, true, base_register, 3,
					4)).reason == arm64::StopReason::continued);
			test::require(get_pair(memory, rcpc::base - total_size,
				element_size) == expected);
			test::require(cpu.registers[base_register] ==
				rcpc::base - total_size);
		}

		for (const bool load : {false, true}) {
			rcpc::Memory memory;
			arm64::CpuState cpu{};
			cpu.pc = rcpc::pc;
			cpu.registers[2] = rcpc::base + 1;
			cpu.registers[3] = replacements[0];
			cpu.registers[4] = replacements[1];
			const auto before = cpu;
			const auto fault = arm64::Dispatcher{}.execute(cpu, memory,
				ordered_pair(size_code, load, false, 2, 3, 4));
			test::require(fault.reason == arm64::StopReason::memory_fault &&
				fault.address == rcpc::base + 1 &&
				fault.write == !load);
			test::require(cpu.pc == before.pc &&
				cpu.registers == before.registers &&
				memory.atomic_pointers == 0);
		}

		/* Pair alignment is the element size, not the combined transfer size. */
		for (const bool load : {false, true}) {
			rcpc::Memory memory;
			const auto address = rcpc::base + element_size;
			put_pair(memory, address, element_size, values);
			arm64::CpuState cpu{};
			cpu.pc = rcpc::pc;
			cpu.registers[2] = address;
			cpu.registers[3] = replacements[0];
			cpu.registers[4] = replacements[1];
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				ordered_pair(size_code, load, false, 2, 3, 4)).reason ==
				arm64::StopReason::continued);
		}
	}

	for (const auto duplicate : {
		ordered_pair(2, true, false, 2, 3, 3),
		ordered_pair(3, true, true, 2, 4, 4)}) {
		rcpc::Memory memory;
		arm64::CpuState cpu{};
		cpu.pc = rcpc::pc;
		test::require(arm64::Dispatcher{}.execute(cpu, memory, duplicate).reason ==
			arm64::StopReason::illegal_instruction);
		test::require(memory.atomic_pointers == 0);
	}

	for (const bool load : {false, true}) {
		rcpc::Memory memory;
		memory.fault = true;
		arm64::CpuState cpu{};
		cpu.pc = rcpc::pc;
		cpu.registers[2] = rcpc::base;
		cpu.registers[3] = replacements[0];
		cpu.registers[4] = replacements[1];
		const auto before = cpu;
		const auto result = arm64::Dispatcher{}.execute(cpu, memory,
			ordered_pair(3, load, false, 2, 3, 4));
		test::require(result.reason == arm64::StopReason::memory_fault &&
			result.write == !load && cpu.pc == before.pc &&
			cpu.registers == before.registers);
	}

	for (const bool direct : {false, true}) {
		rcpc::Memory memory;
		memory.direct = direct;
		put_pair(memory, rcpc::base, 8, values);
		arm64::CpuState cpu{};
		cpu.pc = rcpc::pc;
		cpu.registers[2] = rcpc::base;
		cpu.registers[3] = values[0];
		cpu.registers[4] = values[1];
		cpu.registers[6] = replacements[0];
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			scalar_exclusive(true, 2, 5)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			ordered_pair(3, false, false, 2, 3, 4)).reason ==
			arm64::StopReason::continued);
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			scalar_exclusive(false, 2, 6, 7)).reason ==
			arm64::StopReason::continued);
		test::require(cpu.registers[7] == 1);
	}
}

void exhaustive_gadget_matrix_and_edges()
{
	test::StaticMapping mapping{0x480'0000, 223,
		"/private/tmp/uml-base-ordered-pair.XXXXXX"};
	const auto base = mapping.guest() + 4 * test::StaticMapping::page_size + 512;
	for (unsigned size_code = 2; size_code < 4; ++size_code) {
		const auto element_size = std::size_t{1} << size_code;
		const auto total_size = element_size * 2;
		const auto mask = rcpc::mask(element_size);
		for (const bool load : {false, true}) {
			for (const bool writeback : {false, true}) {
				for (unsigned register_mode = 0; register_mode < 4;
				     ++register_mode) {
					const unsigned first =
						(register_mode & 2U) ? 31U : 3U;
					const unsigned second =
						(register_mode & 1U) ? 31U : 4U;
					const auto address = load || !writeback ? base :
						base - total_size;
					put_mapping_pair(mapping, address, element_size, values);
					arm64::CpuState cpu{};
					cpu.registers[2] = base;
					cpu.registers[3] = replacements[0];
					cpu.registers[4] = replacements[1];
					test::require(mapping.execute(rcpc::pc,
						ordered_pair(size_code, load, writeback, 2, first,
							second), cpu) == gadgets::Event::system_call);
					if (load) {
						if (first != 31U)
							test::require(cpu.registers[first] ==
								(values[0] & mask));
						if (second != 31U)
							test::require(cpu.registers[second] ==
								(values[1] & mask));
					} else {
						const auto actual = get_mapping_pair(mapping, address,
							element_size);
						test::require((actual[0] & mask) ==
							(first == 31U ? 0U : replacements[0] & mask));
						test::require((actual[1] & mask) ==
							(second == 31U ? 0U : replacements[1] & mask));
					}
					test::require(cpu.registers[2] == (writeback ?
						(load ? base + total_size : base - total_size) :
						base));
				}
			}
		}

		for (const bool second_overlap : {false, true}) {
			put_mapping_pair(mapping, base, element_size, values);
			arm64::CpuState cpu{};
			cpu.registers[second_overlap ? 4U : 3U] = base;
			test::require(mapping.execute(rcpc::pc,
				ordered_pair(size_code, true, true,
					second_overlap ? 4U : 3U, 3, 4), cpu) ==
				gadgets::Event::system_call);
			test::require(cpu.registers[3] == (values[0] & mask) &&
				cpu.registers[4] == (values[1] & mask));
		}

		for (const bool second_overlap : {false, true}) {
			arm64::CpuState cpu{};
			const unsigned base_register = second_overlap ? 4U : 3U;
			cpu.registers[base_register] = base;
			cpu.registers[second_overlap ? 3U : 4U] = replacements[
				second_overlap ? 0U : 1U];
			const std::array expected{
				second_overlap ? replacements[0] & mask : base & mask,
				second_overlap ? base & mask : replacements[1] & mask};
			test::require(mapping.execute(rcpc::pc,
				ordered_pair(size_code, false, true, base_register, 3, 4),
				cpu) == gadgets::Event::system_call);
			test::require(get_mapping_pair(mapping, base - total_size,
				element_size) == expected);
			test::require(cpu.registers[base_register] == base - total_size);
		}

		for (const bool load : {false, true}) {
			for (const bool writeback : {false, true}) {
				const auto address = load || !writeback ? base :
					base - total_size;
				put_mapping_pair(mapping, address, element_size, values);
				arm64::CpuState cpu{};
				cpu.sp = base;
				cpu.registers[3] = replacements[0];
				cpu.registers[4] = replacements[1];
				test::require(mapping.execute(rcpc::pc,
					ordered_pair(size_code, load, writeback, 31, 3, 4),
					cpu) == gadgets::Event::system_call);
				test::require(cpu.sp == (writeback ?
					(load ? base + total_size : base - total_size) : base));
			}
		}

		for (const bool load : {false, true}) {
			arm64::CpuState cpu{};
			cpu.registers[2] = base + 1;
			cpu.registers[3] = replacements[0];
			cpu.registers[4] = replacements[1];
			const auto before = cpu.registers;
			test::require(mapping.execute(rcpc::pc,
				ordered_pair(size_code, load, false, 2, 3, 4), cpu) ==
				gadgets::Event::memory_fault);
			test::require(cpu.pc == rcpc::pc && cpu.registers == before);
		}

		for (const bool load : {false, true}) {
			const auto address = base + element_size;
			put_mapping_pair(mapping, address, element_size, values);
			arm64::CpuState cpu{};
			cpu.registers[2] = address;
			cpu.registers[3] = replacements[0];
			cpu.registers[4] = replacements[1];
			test::require(mapping.execute(rcpc::pc,
				ordered_pair(size_code, load, false, 2, 3, 4), cpu) ==
				gadgets::Event::system_call);
		}
	}

	for (const auto duplicate : {
		ordered_pair(2, true, false, 2, 3, 3),
		ordered_pair(3, true, true, 2, 4, 4)}) {
		arm64::CpuState cpu{};
		test::require(mapping.execute(rcpc::pc, duplicate, cpu) ==
			gadgets::Event::illegal_instruction);
	}

	for (const bool load : {false, true}) {
		arm64::CpuState cpu{};
		cpu.registers[2] = mapping.guest() + test::StaticMapping::length;
		const auto before = cpu.registers;
		test::require(mapping.execute(rcpc::pc,
			ordered_pair(3, load, false, 2, 3, 4), cpu) ==
			gadgets::Event::memory_fault);
		test::require(cpu.pc == rcpc::pc && cpu.registers == before);
	}

	/* If only the first element is mapped, pretranslation of the second page
	 * faults before either store can modify memory. */
	const auto boundary = mapping.guest() + test::StaticMapping::length - 8;
	std::memcpy(mapping.at(boundary), &values[0], 8);
	for (const bool load : {false, true}) {
		arm64::CpuState cpu{};
		cpu.registers[2] = boundary;
		cpu.registers[3] = replacements[0];
		cpu.registers[4] = replacements[1];
		const auto before = cpu.registers;
		test::require(mapping.execute(rcpc::pc,
			ordered_pair(3, load, false, 2, 3, 4), cpu) ==
			gadgets::Event::memory_fault);
		test::require(cpu.pc == rcpc::pc && cpu.registers == before);
		std::uint64_t first_actual{};
		std::memcpy(&first_actual, mapping.at(boundary), 8);
		test::require(first_actual == values[0]);
	}

	/* Warm translations, then prove an equal-value STILP invalidates a monitor. */
	arm64::CpuState warm{};
	warm.registers[2] = base;
	warm.registers[3] = values[0];
	test::require(mapping.execute(rcpc::pc,
		test::unsigned_immediate(3, 1, 0, 2, 8), warm) ==
		gadgets::Event::system_call);
	test::require(mapping.execute(rcpc::pc,
		test::unsigned_immediate(3, 0, 0, 2, 3), warm) ==
		gadgets::Event::system_call);
	std::memcpy(mapping.at(base), values.data(), 16);
	arm64::CpuState cpu{};
	cpu.registers[2] = base;
	cpu.registers[3] = values[0];
	cpu.registers[4] = values[1];
	cpu.registers[6] = replacements[0];
	const std::array stream{
		scalar_exclusive(true, 2, 5),
		ordered_pair(3, false, false, 2, 3, 4),
		scalar_exclusive(false, 2, 6, 7),
		0xd400'0001U};
	test::require(mapping.execute(rcpc::pc,
		std::span<const std::uint32_t>{stream}, cpu) ==
		gadgets::Event::system_call);
	test::require(cpu.registers[7] == 1);
}

} // namespace

int main()
{
	exhaustive_interpreter_matrix();
	interpreter_overlaps_faults_and_reservations();
	exhaustive_gadget_matrix_and_edges();
}
