// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_atomic_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_atomic_rmw_engine_test {

namespace atomic = arm64_atomic_engine_test;
namespace arm64 = uml::backend::arm64;
namespace engine = uml::backend::tests;

[[nodiscard]] constexpr bool has_unprivileged_form(unsigned operation)
{
	return operation == 0U || operation == 1U || operation == 3U ||
		operation == 8U;
}

[[nodiscard]] constexpr std::int64_t signed_value(std::uint64_t value,
	std::size_t size)
{
	const auto bits = static_cast<unsigned>(size * 8U);
	if (bits == 64U)
		return static_cast<std::int64_t>(value);
	const auto shift = 64U - bits;
	return static_cast<std::int64_t>(value << shift) >> shift;
}

[[nodiscard]] constexpr std::uint64_t oracle(unsigned operation,
	std::uint64_t observed, std::uint64_t operand, std::size_t size)
{
	const auto mask = atomic::mask_for_size(size);
	observed &= mask;
	operand &= mask;
	switch (operation) {
	case 0U:
		return (observed + operand) & mask;
	case 1U:
		return observed & ~operand & mask;
	case 2U:
		return observed ^ operand;
	case 3U:
		return observed | operand;
	case 4U:
		return signed_value(observed, size) >= signed_value(operand, size) ?
			observed : operand;
	case 5U:
		return signed_value(observed, size) <= signed_value(operand, size) ?
			observed : operand;
	case 6U:
		return observed >= operand ? observed : operand;
	case 7U:
		return observed <= operand ? observed : operand;
	case 8U:
		return operand;
	default:
		return observed;
	}
}

[[nodiscard]] constexpr std::array<std::array<std::uint64_t, 2>, 2>
value_pairs(std::size_t size)
{
	const auto sign = std::uint64_t{1} << (size * 8U - 1U);
	return {{
		{sign | 0x15U, sign - 0x22U},
		{sign - 0x22U, sign | 0x15U},
	}};
}

inline void check_case(engine::EngineHarness &harness,
	std::uint32_t instruction, unsigned operation, std::size_t size,
	unsigned base, unsigned source, unsigned destination,
	const arm64::CpuState &initial, std::uint64_t observed,
	engine::NativeRequirements native, std::string_view name)
{
	atomic::Page initial_page{};
	atomic::put(initial_page, atomic::data_offset, observed, size);
	auto expected_page = initial_page;
	const auto operand = atomic::read_register(initial, source);
	atomic::put(expected_page, atomic::data_offset,
		oracle(operation, observed, operand, size), size);
	const auto memory = atomic::region(initial_page, expected_page);
	auto expected = initial;
	atomic::write_register(expected, destination,
		observed & atomic::mask_for_size(size));
	harness.check({
		.name = name,
		.instruction = instruction,
		.initial = initial,
		.expected = expected,
		.native = native,
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
	(void)base;
}

inline void check_distinct(engine::EngineHarness &harness,
	std::uint32_t instruction, unsigned operation, std::size_t size,
	unsigned base, unsigned source, unsigned destination,
	std::uint64_t observed, std::uint64_t operand,
	engine::NativeRequirements native, std::string_view name)
{
	auto initial = atomic::state(operation * 13U +
		static_cast<unsigned>(size));
	atomic::write_register(initial, source, operand);
	atomic::write_register(initial, base, atomic::data_address, true);
	check_case(harness, instruction, operation, size, base, source,
		destination, initial, observed, native, name);
}

inline void matrix(engine::EngineHarness &harness, unsigned operation)
{
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		for (const bool acquire : {false, true})
			for (const bool release : {false, true})
				for (const auto values : value_pairs(size))
					check_distinct(harness,
						atomic::atomic(size_code, acquire, release,
							operation, 2, 3, 4),
						operation, size, 2, 3, 4, values[0],
						values[1], atomic::lse(),
						"atomic RMW standard matrix");
	}

	if (!has_unprivileged_form(operation))
		return;
	for (unsigned encoded_size = 0; encoded_size < 2; ++encoded_size) {
		const auto size = std::size_t{4} << encoded_size;
		for (const bool acquire : {false, true})
			for (const bool release : {false, true})
				for (const auto values : value_pairs(size))
					check_distinct(harness,
						atomic::unprivileged_atomic(encoded_size,
							acquire, release, operation, 2, 3, 4),
						operation, size, 2, 3, 4, values[0],
						values[1], atomic::lsui(),
						"atomic RMW unprivileged matrix");
	}
}

inline void register_modes(engine::EngineHarness &harness,
	unsigned operation)
{
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		const auto values = value_pairs(size)[0];
		for (const unsigned base : {2U, 31U})
			for (const unsigned source : {3U, 31U})
				for (const unsigned destination : {4U, 31U})
					check_distinct(harness,
						atomic::atomic(size_code, true, true,
							operation, base, source, destination),
						operation, size, base, source, destination,
						values[0], values[1], atomic::lse(),
						"atomic RMW register modes");
	}

	if (!has_unprivileged_form(operation))
		return;
	for (unsigned encoded_size = 0; encoded_size < 2; ++encoded_size) {
		const auto size = std::size_t{4} << encoded_size;
		const auto values = value_pairs(size)[0];
		for (const unsigned base : {2U, 31U})
			for (const unsigned source : {3U, 31U})
				for (const unsigned destination : {4U, 31U})
					check_distinct(harness,
						atomic::unprivileged_atomic(encoded_size,
							true, true, operation, base, source,
							destination),
						operation, size, base, source, destination,
						values[0], values[1], atomic::lsui(),
						"atomic RMW unprivileged register modes");
	}
}

inline void overlaps(engine::EngineHarness &harness, unsigned operation)
{
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		const auto values = value_pairs(size)[0];
		for (const bool acquire : {false, true})
			for (const bool release : {false, true})
				for (const auto registers : {
					std::array{2U, 2U, 4U},
					std::array{2U, 3U, 3U},
					std::array{2U, 3U, 2U},
				}) {
					auto initial = atomic::state(operation + 31U +
						size_code * 4U +
						static_cast<unsigned>(acquire) * 2U +
						static_cast<unsigned>(release));
					atomic::write_register(initial, 3, values[1]);
					atomic::write_register(initial, 2,
						atomic::data_address);
					check_case(harness,
						atomic::atomic(size_code, acquire, release,
							operation, registers[0], registers[1],
							registers[2]),
						operation, size, registers[0], registers[1],
						registers[2], initial, values[0],
						atomic::lse(), "atomic RMW overlap");
				}
	}

	if (!has_unprivileged_form(operation))
		return;
	for (unsigned encoded_size = 0; encoded_size < 2; ++encoded_size) {
		const auto size = std::size_t{4} << encoded_size;
		const auto values = value_pairs(size)[0];
		for (const bool acquire : {false, true})
			for (const bool release : {false, true})
				for (const auto registers : {
					std::array{2U, 2U, 4U},
					std::array{2U, 3U, 3U},
					std::array{2U, 3U, 2U},
				}) {
					auto initial = atomic::state(operation + 47U +
						encoded_size * 4U +
						static_cast<unsigned>(acquire) * 2U +
						static_cast<unsigned>(release));
					atomic::write_register(initial, 3, values[1]);
					atomic::write_register(initial, 2,
						atomic::data_address);
					check_case(harness,
						atomic::unprivileged_atomic(encoded_size,
							acquire, release, operation,
							registers[0], registers[1],
							registers[2]),
						operation, size, registers[0], registers[1],
						registers[2], initial, values[0],
						atomic::lsui(),
						"atomic RMW unprivileged overlap");
				}
	}
}

inline void fault_case(engine::EngineHarness &harness,
	std::uint32_t instruction, arm64::CpuState initial,
	std::uint64_t address, engine::NativeRequirements native,
	std::string_view name, std::span<const engine::MemoryRegion> memory = {})
{
	harness.check({
		.name = name,
		.instruction = instruction,
		.initial = initial,
		.expected = initial,
		.event = engine::ExpectedEvent::memory_fault,
		.native = native,
		.fault = {
			.address = address,
			.access = arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = memory,
	});
}

inline void faults(engine::EngineHarness &harness, unsigned operation)
{
	atomic::Page page{};
	atomic::put(page, atomic::data_offset, 0x1234'5678'9abc'def0ULL, 8);
	const auto mapped = atomic::region(page, page);
	const std::span<const engine::MemoryRegion> mapping{&mapped, 1};

	for (unsigned size_code = 1; size_code < 4; ++size_code) {
		auto initial = atomic::state(operation + size_code);
		initial.registers[2] = atomic::data_address + 1;
		initial.registers[3] = 7;
		fault_case(harness,
			atomic::atomic(size_code, true, true, operation, 2, 3, 4),
			initial, atomic::data_address + 1, atomic::lse(),
			"atomic RMW natural alignment", mapping);
	}
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		auto initial = atomic::state(operation + size_code + 9U);
		initial.sp = atomic::data_address + 8;
		initial.registers[3] = 7;
		fault_case(harness,
			atomic::atomic(size_code, true, true, operation, 31, 3, 4),
			initial, atomic::data_address + 8, atomic::lse(),
			"atomic RMW SP alignment", mapping);
	}

	auto read_only_state = atomic::state(operation + 19U);
	read_only_state.registers[2] = atomic::data_address;
	read_only_state.registers[3] = 7;
	const auto read_only = atomic::region(page, page, true, false);
	fault_case(harness,
		atomic::atomic(3, true, true, operation, 2, 3, 4),
		read_only_state, atomic::data_address, atomic::lse(),
		"atomic RMW read-only",
		std::span<const engine::MemoryRegion>{&read_only, 1});

	auto unmapped = atomic::state(operation + 23U);
	unmapped.registers[2] =
		atomic::data_page + DARWIN_UML_GUEST_PAGE_SIZE * 4ULL;
	unmapped.registers[3] = 7;
	fault_case(harness,
		atomic::atomic(3, true, true, operation, 2, 3, 4), unmapped,
		unmapped.registers[2], atomic::lse(), "atomic RMW unmapped");

	if (!has_unprivileged_form(operation))
		return;
	for (unsigned encoded_size = 0; encoded_size < 2; ++encoded_size) {
		auto initial = atomic::state(operation + encoded_size + 29U);
		initial.registers[2] = atomic::data_address + 1;
		initial.registers[3] = 7;
		fault_case(harness,
			atomic::unprivileged_atomic(encoded_size, true, true,
				operation, 2, 3, 4),
			initial, atomic::data_address + 1, atomic::lsui(),
			"atomic RMW unprivileged natural alignment", mapping);
		initial = atomic::state(operation + encoded_size + 37U);
		initial.sp = atomic::data_address + 8;
		initial.registers[3] = 7;
		fault_case(harness,
			atomic::unprivileged_atomic(encoded_size, true, true,
				operation, 31, 3, 4),
			initial, atomic::data_address + 8, atomic::lsui(),
			"atomic RMW unprivileged SP alignment", mapping);
	}

	read_only_state = atomic::state(operation + 43U);
	read_only_state.registers[2] = atomic::data_address;
	read_only_state.registers[3] = 7;
	fault_case(harness,
		atomic::unprivileged_atomic(1, true, true, operation, 2, 3, 4),
		read_only_state, atomic::data_address, atomic::lsui(),
		"atomic RMW unprivileged read-only",
		std::span<const engine::MemoryRegion>{&read_only, 1});
	unmapped = atomic::state(operation + 53U);
	unmapped.registers[2] =
		atomic::data_page + DARWIN_UML_GUEST_PAGE_SIZE * 4ULL;
	unmapped.registers[3] = 7;
	fault_case(harness,
		atomic::unprivileged_atomic(1, true, true, operation, 2, 3, 4),
		unmapped, unmapped.registers[2], atomic::lsui(),
		"atomic RMW unprivileged unmapped");
}

inline void reservation_invalidation(engine::EngineHarness &harness,
	unsigned operation)
{
	constexpr std::uint64_t observed = 0x1234'5678'9abc'def0ULL;
	const auto operand = operation == 4U || operation == 5U ||
		operation == 6U || operation == 7U || operation == 8U ?
		observed : 0U;
	atomic::Page page{};
	atomic::put(page, atomic::data_offset, observed, 8);
	const auto memory = atomic::region(page, page);
	const std::array code{
		engine::ProgramWord{0,
			atomic::scalar_exclusive(3, true, false, 2, 5)},
		engine::ProgramWord{4,
			atomic::atomic(3, false, false, operation, 2, 3, 31)},
		engine::ProgramWord{8,
			atomic::scalar_exclusive(3, false, false, 2, 4, 6)},
		engine::ProgramWord{12, atomic::clrex(3)},
		engine::ProgramWord{16, atomic::terminal_system_call},
	};
	auto initial = atomic::state(operation + 61U);
	initial.registers[2] = atomic::data_address;
	initial.registers[3] = operand;
	initial.registers[4] = 0x0fed'cba9'8765'4321ULL;
	auto expected = initial;
	expected.registers[5] = observed;
	expected.registers[6] = 1;
	atomic::clear_monitor(expected);
	expected.pc = engine::engine_test_code_base + 20;
	harness.check_program({
		.name = "atomic RMW invalidates reservation",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = atomic::lse(),
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
}

inline void invalid_encodings(engine::EngineHarness &harness)
{
	auto initial = atomic::state(91);
	initial.registers[2] = atomic::data_address;
	/*
	 * The o3=1 space is shared with other architectural families.  In
	 * particular, opcodes 1..3 at size codes 0 and 1 are read-check-write
	 * operations, while opcodes 2 and 3 at size code 3 are 64-byte stores.
	 * Exercise only combinations which the allocation tables mark
	 * unallocated.
	 */
	for (const unsigned size_code : {0U, 1U})
		for (const unsigned opcode : {4U, 5U, 6U, 7U})
			harness.check_illegal(
				"atomic RMW reserved narrow standard operation",
				atomic::atomic_raw(size_code, false, false, true,
					opcode, 2, 3, 4),
				initial, atomic::baseline());
	for (unsigned opcode = 1; opcode < 8; ++opcode)
		harness.check_illegal("atomic RMW reserved word operation",
			atomic::atomic_raw(2, false, false, true, opcode, 2, 3, 4),
			initial, atomic::baseline());
	for (const unsigned opcode : {1U, 4U, 5U, 6U, 7U})
		harness.check_illegal(
			"atomic RMW reserved doubleword standard operation",
			atomic::atomic_raw(3, false, false, true, opcode, 2, 3, 4),
			initial, atomic::baseline());

	/*
	 * LSUI has a one-bit size field.  Setting bit 31 as though it were a
	 * second size bit changes the top-level allocation.  The size-code-two
	 * field pattern below is unallocated, whereas size code three with the
	 * same remaining fields is a valid memory-tag operation and must not be
	 * diagnosed as a reserved LSUI form.
	 */
	harness.check_illegal("atomic RMW reserved unprivileged size",
		atomic::unprivileged_atomic_raw(2, false, false, false, 0, 2, 3, 4),
		initial, atomic::baseline());
	for (const unsigned opcode : {2U, 4U, 5U, 6U, 7U})
		harness.check_illegal("atomic RMW reserved unprivileged operation",
			atomic::unprivileged_atomic_raw(0, false, false, false,
				opcode, 2, 3, 4),
			initial, atomic::baseline());
	for (unsigned opcode = 1; opcode < 8; ++opcode)
		harness.check_illegal("atomic RMW reserved unprivileged o3 operation",
			atomic::unprivileged_atomic_raw(0, false, false, true,
				opcode, 2, 3, 4),
			initial, atomic::baseline());
}

inline void run_operation(engine::EngineHarness &harness, unsigned operation)
{
	matrix(harness, operation);
	register_modes(harness, operation);
	overlaps(harness, operation);
	faults(harness, operation);
	reservation_invalidation(harness, operation);
}

} // namespace arm64_atomic_rmw_engine_test
