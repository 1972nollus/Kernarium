// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_test_support.hpp"
#include "arm64_pauth_test_support.hpp"

#include <cstring>

namespace load_test = arm64_load_store_test;
namespace pauth_test = arm64_pauth_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x4e0'0000;

[[nodiscard]] constexpr std::uint32_t ldraa(
	bool key_b, bool writeback, unsigned encoded_offset,
	unsigned base, unsigned destination)
{
	return 0xf820'0400U | (static_cast<unsigned>(key_b) << 23) |
		(((encoded_offset >> 9) & 1U) << 22) |
		((encoded_offset & 0x1ffU) << 12) |
		(static_cast<unsigned>(writeback) << 11) |
		((base & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] std::uint64_t signed_base(std::uint64_t address, bool key_b)
{
	const auto initial = pauth_test::state(address, 0);
	return pauth_test::execute_both(
		pauth_test::data_processing(key_b ? 11U : 10U, 0), initial)
		.interpreted.registers[0];
}

[[nodiscard]] arm64::CpuState initial_state(std::uint64_t base_value,
	unsigned base, unsigned destination)
{
	arm64::CpuState cpu{};
	load_test::fill_registers(cpu);
	if (base < 31)
		cpu.registers[base] = base_value;
	else
		cpu.sp = base_value;
	if (destination < 31 && destination != base)
		cpu.registers[destination] = 0xdead'beef'0123'4567ULL;
	return cpu;
}

void successful_forms()
{
	constexpr std::uint64_t guest = 0x3a00'0000;
	load_test::StaticMapping mapping{guest, 1,
		"/tmp/uml-pauth-load-success-XXXXXX"};
	constexpr std::array encoded_offsets{0U, 1U, 0x1ffU,
		0x200U, 0x3ffU, 0x1feU};
	constexpr std::array bases{0U, 17U, 30U, 31U};
	constexpr std::array destinations{0U, 7U, 30U, 31U};
	constexpr std::uint64_t payload = 0xe817'4675'a4d3'fe81ULL;

	for (const bool key_b : {false, true})
		for (const bool writeback : {false, true})
			for (const unsigned base : bases)
				for (const unsigned destination : destinations) {
					if (writeback && base == destination && base != 31U)
						continue;
					for (const unsigned encoded_offset : encoded_offsets) {
						const auto displacement =
							load_test::sign_extend(encoded_offset, 10) * 8;
						const auto plain_base = guest + 12 * 4096;
						const auto address = plain_base + displacement;
						std::memcpy(mapping.at(address), &payload, sizeof(payload));
						const auto signed_address = signed_base(plain_base, key_b);
						const auto initial = initial_state(
							signed_address, base, destination);
						const auto instruction = ldraa(key_b, writeback,
							encoded_offset, base, destination);

						load_test::ProbeMemory memory;
						std::memcpy(memory.payload.data(), &payload,
							sizeof(payload));
						auto interpreted = initial;
						interpreted.pc = pc;
						const auto result = arm64::Dispatcher{}.execute(
							interpreted, memory, instruction);
						load_test::require(result.reason ==
							arm64::StopReason::continued);
						load_test::require(memory.reads == 1 &&
							memory.last_address == address && memory.last_size == 8);

						auto gadget = initial;
						load_test::require(mapping.execute(pc, instruction, gadget) ==
							gadgets::Event::system_call);
						if (destination < 31) {
							load_test::require(interpreted.registers[destination] ==
								payload);
							load_test::require(gadget.registers[destination] == payload);
						}
						if (writeback) {
							load_test::require(interpreted.read_register(base, true) ==
								address);
							load_test::require(gadget.read_register(base, true) == address);
						} else if (base != destination || base == 31U) {
							load_test::require(interpreted.read_register(base, true) ==
								signed_address);
							load_test::require(gadget.read_register(base, true) ==
								signed_address);
						}
					}
				}
}

void cross_page_load()
{
	constexpr std::uint64_t guest = 0x3b00'0000;
	load_test::StaticMapping mapping{guest, 2,
		"/tmp/uml-pauth-load-cross-XXXXXX"};
	constexpr std::uint64_t address = guest + 4 * 4096 - 4;
	constexpr std::uint64_t payload = 0x0123'4567'89ab'cdefULL;
	std::memcpy(mapping.at(address), &payload, sizeof(payload));
	for (const bool key_b : {false, true}) {
		auto cpu = initial_state(signed_base(address, key_b), 3, 7);
		load_test::require(mapping.execute(
			pc, ldraa(key_b, false, 0, 3, 7), cpu) ==
			gadgets::Event::system_call);
		load_test::require(cpu.registers[7] == payload);
	}
}

void fault_atomicity()
{
	for (const bool key_b : {false, true})
		for (const bool writeback : {false, true}) {
			const auto initial = initial_state(
				signed_base(0x7a00'1000, key_b), 3, 7);
			const auto instruction = ldraa(key_b, writeback, 1, 3, 7);
			load_test::ProbeMemory memory;
			memory.fault_read = true;
			auto interpreted = initial;
			interpreted.pc = pc;
			load_test::require(arm64::Dispatcher{}.execute(
				interpreted, memory, instruction).reason ==
				arm64::StopReason::memory_fault);
			load_test::require(interpreted.registers == initial.registers);

			load_test::StaticMapping mapping{0x3c00'0000,
				static_cast<unsigned long>(3 + key_b * 2 + writeback),
				"/tmp/uml-pauth-load-fault-XXXXXX"};
			auto gadget = initial;
			load_test::require(mapping.execute(pc, instruction, gadget) ==
				gadgets::Event::memory_fault);
			load_test::require(gadget.registers == initial.registers);
			load_test::require(gadget.pc == pc);
		}
}

void sp_alignment_fault()
{
	for (const bool key_b : {false, true})
		for (const bool writeback : {false, true}) {
			const auto initial = initial_state(
				signed_base(0x3d00'1008, key_b), 31, 7);
			const auto instruction = ldraa(key_b, writeback, 0, 31, 7);
			load_test::ProbeMemory memory;
			auto interpreted = initial;
			interpreted.pc = pc;
			load_test::require(arm64::Dispatcher{}.execute(
				interpreted, memory, instruction).reason ==
				arm64::StopReason::memory_fault);
			load_test::require(memory.reads == 0);

			load_test::StaticMapping mapping{0x3d00'0000,
				static_cast<unsigned long>(8 + key_b * 2 + writeback),
				"/tmp/uml-pauth-load-align-XXXXXX"};
			auto gadget = initial;
			load_test::require(mapping.execute(pc, instruction, gadget) ==
				gadgets::Event::memory_fault);
			load_test::require(gadget.registers == initial.registers);
			load_test::require(gadget.sp == initial.sp);
		}
}

void writeback_overlap_is_undefined()
{
	for (const bool key_b : {false, true})
		for (unsigned base = 0; base < 31; ++base)
			for (const unsigned encoded_offset : {0U, 0x1ffU, 0x200U})
				pauth_test::require_illegal(ldraa(
					key_b, true, encoded_offset, base, base));
}

void production_specialization()
{
#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	const auto a = gadgets::Builder{}.build(pc,
		std::array{ldraa(false, false, 0, 3, 7)});
	const auto b = gadgets::Builder{}.build(pc,
		std::array{ldraa(true, false, 0, 3, 7)});
	const auto writeback = gadgets::Builder{}.build(pc,
		std::array{ldraa(false, true, 0, 3, 7)});
	const auto sp = gadgets::Builder{}.build(pc,
		std::array{ldraa(false, false, 0, 31, 7)});
	const auto discard = gadgets::Builder{}.build(pc,
		std::array{ldraa(false, false, 0, 3, 31)});
	load_test::require(a.program.words().front() != b.program.words().front());
	load_test::require(a.program.words().front() !=
		writeback.program.words().front());
	load_test::require(a.program.words().front() != sp.program.words().front());
	load_test::require(a.program.words().front() != discard.program.words().front());
#endif
}

} // namespace

int main()
{
	successful_forms();
	cross_page_load();
	fault_atomicity();
	sp_alignment_fault();
	writeback_overlap_is_undefined();
	production_specialization();
	return 0;
}
