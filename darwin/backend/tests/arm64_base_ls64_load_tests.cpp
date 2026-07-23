// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_ls64_test_support.hpp"

#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstdint>
#include <memory>
#include <set>

namespace test = ls64_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

void encoding_examples()
{
	test::test::require(test::instruction(
		test::Operation::load, 0, 1) == 0xf83f'd020U);
	test::test::require(test::instruction(
		test::Operation::load, 22, 31) == 0xf83f'd3f6U);
}

void interpreter_register_and_address_encodings()
{
	test::ProbeMemory memory;
	test::fill_payload(memory.payload);
	for (unsigned first = 0; first < 24; first += 2)
		for (unsigned base = 0; base < 32; ++base) {
			auto cpu = test::state();
			test::set_base(cpu, base, test::interpreter_address);
			cpu.pc = test::pc;
			auto expected = cpu;
			test::apply_expected_load(expected, first, memory.payload);
			memory.reset_counts();
			const auto encoded = test::instruction(
				test::Operation::load, first, base);
			const auto result = arm64::Dispatcher{}.execute(
				cpu, memory, encoded);
			test::test::require(result.reason == arm64::StopReason::continued &&
				result.instructions == 1 && result.address == test::pc &&
				result.instruction == encoded && !result.write);
			test::test::require(memory.reads == 1 && memory.writes == 0 &&
				memory.last_address == test::interpreter_address &&
				memory.last_size == 64);
			test::require_state(cpu, expected, test::pc + 4);
		}
}

void gadget_register_and_address_encodings()
{
	constexpr std::uint64_t guest = 0x6d00'0000ULL;
	test::test::StaticMapping mapping{
		guest, 0x6c6401UL, "/tmp/uml-ls64-load-XXXXXX"};
	const auto address = guest + 2 * test::test::StaticMapping::page_size;
	std::array<std::byte, 64> payload{};
	test::fill_payload(payload, 3);
	for (unsigned first = 0; first < 24; first += 2)
		for (unsigned base = 0; base < 32; ++base) {
			std::copy(payload.begin(), payload.end(), mapping.at(address));
			auto cpu = test::state();
			test::set_base(cpu, base, address);
			auto expected = cpu;
			test::apply_expected_load(expected, first, payload);
			test::test::require(mapping.execute(test::pc,
				test::instruction(test::Operation::load, first, base), cpu) ==
				gadgets::Event::system_call);
			test::require_state(cpu, expected, test::pc + 8);
		}
}

void interpreter_fault_safety()
{
	test::ProbeMemory memory;
	test::fill_payload(memory.payload, 5);

	auto misaligned = test::state();
	test::set_base(misaligned, 3, test::interpreter_address + 1);
	misaligned.pc = test::pc;
	const auto before_misaligned = misaligned;
	const auto alignment = arm64::Dispatcher{}.execute(misaligned, memory,
		test::instruction(test::Operation::load, 4, 3));
	test::test::require(alignment.reason == arm64::StopReason::memory_fault &&
		alignment.instructions == 0 &&
		alignment.address == test::interpreter_address + 1 &&
		!alignment.write && memory.reads == 0 && memory.writes == 0);
	test::require_state(misaligned, before_misaligned, test::pc);

	memory.fault_read = true;
	auto faulting = test::state();
	test::set_base(faulting, 31, test::interpreter_address);
	faulting.pc = test::pc;
	const auto before_fault = faulting;
	const auto fault = arm64::Dispatcher{}.execute(faulting, memory,
		test::instruction(test::Operation::load, 22, 31));
	test::test::require(fault.reason == arm64::StopReason::memory_fault &&
		fault.instructions == 0 && fault.address == test::interpreter_address &&
		!fault.write && memory.reads == 1 && memory.writes == 0 &&
		memory.last_size == 64);
	test::require_state(faulting, before_fault, test::pc);
}

void gadget_fault_safety()
{
	constexpr std::uint64_t guest = 0x6e00'0000ULL;
	test::test::StaticMapping mapping{
		guest, 0x6c6402UL, "/tmp/uml-ls64-load-fault-XXXXXX"};

	auto misaligned = test::state();
	test::set_base(misaligned, 5, guest + 65);
	const auto before_misaligned = misaligned;
	test::test::require(mapping.execute(test::pc,
		test::instruction(test::Operation::load, 2, 5), misaligned) ==
		gadgets::Event::memory_fault);
	test::require_state(misaligned, before_misaligned, test::pc);

	auto unmapped = test::state();
	test::set_base(unmapped, 31,
		guest + test::test::StaticMapping::length);
	const auto before_unmapped = unmapped;
	test::test::require(mapping.execute(test::pc,
		test::instruction(test::Operation::load, 20, 31), unmapped) ==
		gadgets::Event::memory_fault);
	test::require_state(unmapped, before_unmapped, test::pc);
}

void invalid_register_encodings()
{
	test::ProbeMemory memory;
	memory.fault_read = true;
	constexpr std::uint64_t guest = 0x6f00'0000ULL;
	test::test::StaticMapping mapping{
		guest, 0x6c6403UL, "/tmp/uml-ls64-load-invalid-XXXXXX"};
	for (unsigned first = 0; first < 32; ++first) {
		if (test::valid_first(first))
			continue;
		for (unsigned base = 0; base < 32; ++base) {
			const auto encoded = test::instruction(
				test::Operation::load, first, base);
			auto interpreted = test::state();
			test::set_base(interpreted, base, test::interpreter_address + 1);
			interpreted.pc = test::pc;
			const auto before_interpreted = interpreted;
			memory.reset_counts();
			const auto result = arm64::Dispatcher{}.execute(
				interpreted, memory, encoded);
			test::test::require(
				result.reason == arm64::StopReason::illegal_instruction &&
				result.instructions == 0 && result.address == test::pc &&
				memory.reads == 0 && memory.writes == 0);
			test::require_state(
				interpreted, before_interpreted, test::pc);

			auto gadget = test::state();
			test::set_base(gadget, base, guest + 1);
			const auto before_gadget = gadget;
			test::test::require(mapping.execute(test::pc, encoded, gadget) ==
				gadgets::Event::illegal_instruction);
			test::require_state(gadget, before_gadget, test::pc);
		}
	}
}

void production_specialization()
{
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
	return;
#else
	std::set<std::uintptr_t> entries;
	for (const unsigned base : {1U, 31U}) {
		const auto built = gadgets::Builder{}.build(test::pc,
			std::array{test::instruction(test::Operation::load, 0, base)});
		test::test::require(built.complete &&
			built.program.instruction_count() == 1 &&
			built.program.words().size() == 5);
		entries.insert(built.program.words().front());
	}
	test::test::require(entries.size() == 2);
#endif
}

} // namespace

int main()
{
	encoding_examples();
	interpreter_register_and_address_encodings();
	gadget_register_and_address_encodings();
	interpreter_fault_safety();
	gadget_fault_safety();
	invalid_register_encodings();
	production_specialization();
}
