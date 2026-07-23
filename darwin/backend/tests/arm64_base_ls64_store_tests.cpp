// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_ls64_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>

namespace test = ls64_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::array store_operations{
	test::Operation::store,
	test::Operation::store_status,
	test::Operation::store_status_accdata,
};

[[nodiscard]] constexpr bool has_status(test::Operation operation) noexcept
{
	return operation != test::Operation::store;
}

[[nodiscard]] constexpr bool uses_accdata(test::Operation operation) noexcept
{
	return operation == test::Operation::store_status_accdata;
}

void encoding_examples()
{
	test::test::require(test::instruction(
		test::Operation::store, 0, 1) == 0xf83f'9020U);
	test::test::require(test::instruction(
		test::Operation::store, 22, 31) == 0xf83f'93f6U);
	test::test::require(test::instruction(
		test::Operation::store_status, 0, 1, 2) == 0xf822'b020U);
	test::test::require(test::instruction(
		test::Operation::store_status, 22, 31, 31) == 0xf83f'b3f6U);
	test::test::require(test::instruction(
		test::Operation::store_status_accdata, 0, 1, 2) == 0xf822'a020U);
	test::test::require(test::instruction(
		test::Operation::store_status_accdata,
		22, 31, 31) == 0xf83f'a3f6U);
}

void interpreter_register_encodings()
{
	test::ProbeMemory memory;
	for (const auto operation : store_operations)
		for (unsigned first = 0; first < 24; first += 2)
			for (unsigned base = 0; base < 32; ++base) {
				const unsigned status_count = has_status(operation) ? 32U : 1U;
				for (unsigned status_index = 0;
				     status_index < status_count; ++status_index) {
					const unsigned status = has_status(operation) ?
						status_index : 31U;
					auto cpu = test::state();
					test::set_base(
						cpu, base, test::interpreter_address);
					cpu.pc = test::pc;
					const auto expected_bytes = test::expected_store(
						cpu, first, uses_accdata(operation));
					auto expected = cpu;
					if (has_status(operation) && status != 31)
						expected.registers[status] = 0;
					memory.written.fill(std::byte{0xcc});
					memory.reset_counts();
					const auto encoded = test::instruction(
						operation, first, base, status);
					const auto result = arm64::Dispatcher{}.execute(
						cpu, memory, encoded);
					test::test::require(
						result.reason == arm64::StopReason::continued &&
						result.instructions == 1 &&
						result.address == test::pc &&
						result.instruction == encoded && !result.write);
					test::test::require(memory.reads == 0 &&
						memory.writes == 1 &&
						memory.last_address == test::interpreter_address &&
						memory.last_size == 64 &&
						memory.written == expected_bytes);
					test::require_state(cpu, expected, test::pc + 4);
				}
			}
}

void gadget_register_encodings()
{
	constexpr std::uint64_t guest = 0x7000'0000ULL;
	test::test::StaticMapping mapping{
		guest, 0x6c6411UL, "/tmp/uml-ls64-store-XXXXXX"};
	const auto address = guest + 2 * test::test::StaticMapping::page_size;
	for (const auto operation : store_operations)
		for (unsigned first = 0; first < 24; first += 2)
			for (unsigned base = 0; base < 32; ++base) {
				const unsigned status_count = has_status(operation) ? 32U : 1U;
				for (unsigned status_index = 0;
				     status_index < status_count; ++status_index) {
					const unsigned status = has_status(operation) ?
						status_index : 31U;
					auto cpu = test::state();
					test::set_base(cpu, base, address);
					const auto expected_bytes = test::expected_store(
						cpu, first, uses_accdata(operation));
					auto expected = cpu;
					if (has_status(operation) && status != 31)
						expected.registers[status] = 0;
					std::fill_n(mapping.at(address), 64, std::byte{0xcc});
					test::test::require(mapping.execute(test::pc,
						test::instruction(operation, first, base, status),
						cpu) == gadgets::Event::system_call);
					test::test::require(std::equal(expected_bytes.begin(),
						expected_bytes.end(), mapping.at(address)));
					test::require_state(cpu, expected, test::pc + 8);
				}
			}
}

void interpreter_fault_safety()
{
	for (const auto operation : store_operations) {
		const unsigned status = has_status(operation) ? 7U : 31U;
		test::ProbeMemory memory;
		memory.written.fill(std::byte{0xcc});
		auto misaligned = test::state();
		test::set_base(misaligned, 3, test::interpreter_address + 1);
		misaligned.pc = test::pc;
		const auto before_misaligned = misaligned;
		const auto alignment = arm64::Dispatcher{}.execute(
			misaligned, memory,
			test::instruction(operation, 4, 3, status));
		test::test::require(
			alignment.reason == arm64::StopReason::memory_fault &&
			alignment.instructions == 0 &&
			alignment.address == test::interpreter_address + 1 &&
			alignment.write && memory.reads == 0 && memory.writes == 0 &&
			std::all_of(memory.written.begin(), memory.written.end(),
				[](std::byte byte) { return byte == std::byte{0xcc}; }));
		test::require_state(misaligned, before_misaligned, test::pc);

		memory.fault_write = true;
		auto faulting = test::state();
		test::set_base(faulting, 31, test::interpreter_address);
		faulting.pc = test::pc;
		const auto before_fault = faulting;
		const auto fault = arm64::Dispatcher{}.execute(faulting, memory,
			test::instruction(operation, 22, 31, status));
		test::test::require(fault.reason == arm64::StopReason::memory_fault &&
			fault.instructions == 0 &&
			fault.address == test::interpreter_address && fault.write &&
			memory.reads == 0 && memory.writes == 1 &&
			memory.last_size == 64 &&
			std::all_of(memory.written.begin(), memory.written.end(),
				[](std::byte byte) { return byte == std::byte{0xcc}; }));
		test::require_state(faulting, before_fault, test::pc);
	}
}

void gadget_fault_safety()
{
	constexpr std::uint64_t guest = 0x7100'0000ULL;
	test::test::StaticMapping mapping{
		guest, 0x6c6412UL, "/tmp/uml-ls64-store-fault-XXXXXX"};
	const auto aligned = guest + 2 * test::test::StaticMapping::page_size;
	for (const auto operation : store_operations) {
		const unsigned status = has_status(operation) ? 7U : 31U;
		std::fill_n(mapping.at(aligned), 64, std::byte{0xcc});
		auto misaligned = test::state();
		test::set_base(misaligned, 5, aligned + 1);
		const auto before_misaligned = misaligned;
		test::test::require(mapping.execute(test::pc,
			test::instruction(operation, 2, 5, status), misaligned) ==
			gadgets::Event::memory_fault);
		test::require_state(misaligned, before_misaligned, test::pc);
		test::test::require(std::all_of(mapping.at(aligned),
			mapping.at(aligned) + 64,
			[](std::byte byte) { return byte == std::byte{0xcc}; }));

		auto unmapped = test::state();
		test::set_base(unmapped, 31,
			guest + test::test::StaticMapping::length);
		const auto before_unmapped = unmapped;
		test::test::require(mapping.execute(test::pc,
			test::instruction(operation, 20, 31, status), unmapped) ==
			gadgets::Event::memory_fault);
		test::require_state(unmapped, before_unmapped, test::pc);
	}
}

void invalid_register_encodings()
{
	test::ProbeMemory memory;
	memory.fault_write = true;
	constexpr std::uint64_t guest = 0x7200'0000ULL;
	test::test::StaticMapping mapping{
		guest, 0x6c6413UL, "/tmp/uml-ls64-store-invalid-XXXXXX"};
	for (const auto operation : store_operations)
		for (unsigned first = 0; first < 32; ++first) {
			if (test::valid_first(first))
				continue;
			for (unsigned base = 0; base < 32; ++base) {
				const unsigned status_count = has_status(operation) ? 32U : 1U;
				for (unsigned status_index = 0;
				     status_index < status_count; ++status_index) {
					const unsigned status = has_status(operation) ?
						status_index : 31U;
					const auto encoded = test::instruction(
						operation, first, base, status);
					auto interpreted = test::state();
					test::set_base(interpreted, base,
						test::interpreter_address + 1);
					interpreted.pc = test::pc;
					const auto before_interpreted = interpreted;
					memory.reset_counts();
					const auto result = arm64::Dispatcher{}.execute(
						interpreted, memory, encoded);
					test::test::require(result.reason ==
						arm64::StopReason::illegal_instruction &&
						result.instructions == 0 &&
						result.address == test::pc &&
						memory.reads == 0 && memory.writes == 0);
					test::require_state(
						interpreted, before_interpreted, test::pc);

					auto gadget = test::state();
					test::set_base(gadget, base, guest + 1);
					const auto before_gadget = gadget;
					test::test::require(mapping.execute(
						test::pc, encoded, gadget) ==
						gadgets::Event::illegal_instruction);
					test::require_state(gadget, before_gadget, test::pc);
				}
			}
		}
}

void production_specialization()
{
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
	return;
#else
	std::set<std::uintptr_t> entries;
	for (const auto operation : store_operations)
		for (const unsigned base : {1U, 31U}) {
			const unsigned status_count = has_status(operation) ? 2U : 1U;
			for (unsigned status_index = 0;
			     status_index < status_count; ++status_index) {
				const unsigned status = status_index == 0 ? 2U : 31U;
				const auto built = gadgets::Builder{}.build(test::pc,
					std::array{test::instruction(
						operation, 0, base, status)});
				test::test::require(built.complete &&
					built.program.instruction_count() == 1 &&
					built.program.words().size() == 5);
				entries.insert(built.program.words().front());
			}
		}
	test::test::require(entries.size() == 10);
#endif
}

} // namespace

int main()
{
	encoding_examples();
	interpreter_register_encodings();
	gadget_register_encodings();
	interpreter_fault_safety();
	gadget_fault_safety();
	invalid_register_encodings();
	production_specialization();
}
