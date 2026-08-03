// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_rcw_test_support.hpp"

#include <array>

namespace rcw = arm64_base_rcw_test;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

namespace {

constexpr rcw::Pair observed_pair{
	rcw::observed_value,
	rcw::observed_value2,
};
constexpr rcw::Pair operand_pair{
	rcw::operand_value,
	rcw::operand_value2,
};

[[nodiscard]] rcw::arm64::CpuState scalar_initial(
	rcw::Operation operation, unsigned rn, std::uint64_t address,
	std::uint64_t observed, std::uint64_t operand, unsigned seed)
{
	auto initial = atomic::state(seed);
	initial.registers[3] = operand;
	if (operation == rcw::Operation::compare_swap)
		initial.registers[4] = observed;
	atomic::write_register(initial, rn, address, true);
	return initial;
}

[[nodiscard]] rcw::arm64::CpuState pair_initial(
	rcw::Operation operation, unsigned rn, std::uint64_t address,
	const rcw::Pair &old, const rcw::Pair &source, unsigned seed)
{
	auto initial = atomic::state(seed);
	if (operation == rcw::Operation::compare_swap) {
		rcw::write_pair(initial, 2, source);
		rcw::write_pair(initial, 4, old);
	} else {
		initial.registers[2] = source[0];
		initial.registers[4] = source[1];
	}
	atomic::write_register(initial, rn, address, true);
	return initial;
}

void scalar_fault(engine::EngineHarness &harness, rcw::Operation operation,
	bool soft, unsigned rn, std::uint64_t address,
	const engine::MemoryRegion *memory, std::string_view name)
{
	const auto initial = scalar_initial(operation, rn, address,
		rcw::observed_value, rcw::operand_value, 20U +
			rcw::code(operation) * 4U + static_cast<unsigned>(soft));
	harness.check({
		.name = name,
		.instruction = rcw::scalar_instruction(operation, soft, true, true,
			rn, 3, 4),
		.initial = initial,
		.expected = initial,
		.event = engine::ExpectedEvent::memory_fault,
		.native = rcw::scalar_native(),
		.fault = {
			.address = address,
			.access = rcw::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = memory == nullptr ?
			std::span<const engine::MemoryRegion>{} :
			std::span<const engine::MemoryRegion>{memory, 1},
	});
}

void pair_fault(engine::EngineHarness &harness, rcw::Operation operation,
	bool soft, unsigned rn, std::uint64_t address,
	const engine::MemoryRegion *memory, std::string_view name)
{
	const auto initial = pair_initial(operation, rn, address, observed_pair,
		operand_pair, 60U + rcw::code(operation) * 4U +
			static_cast<unsigned>(soft));
	harness.check({
		.name = name,
		.instruction = rcw::pair_instruction(operation, soft, true, true,
			rn, 2, 4),
		.initial = initial,
		.expected = initial,
		.event = engine::ExpectedEvent::memory_fault,
		.native = rcw::pair_native(),
		.fault = {
			.address = address,
			.access = rcw::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = memory == nullptr ?
			std::span<const engine::MemoryRegion>{} :
			std::span<const engine::MemoryRegion>{memory, 1},
	});
}

void scalar_alignment_mapping_and_permissions(engine::EngineHarness &harness)
{
	atomic::Page page{};
	atomic::put(page, atomic::data_offset, rcw::observed_value, 8);
	const auto unchanged = engine::MemoryRegion{
		.address = atomic::data_page,
		.initial = page,
		.expected = page,
	};
	const auto read_only = engine::MemoryRegion{
		.address = atomic::data_page,
		.initial = page,
		.expected = page,
		.readable = true,
		.writable = false,
	};
	const auto inaccessible = engine::MemoryRegion{
		.address = atomic::data_page,
		.initial = page,
		.expected = page,
		.readable = false,
		.writable = false,
	};
	for (const auto operation : {
	     rcw::Operation::compare_swap,
	     rcw::Operation::clear,
	     rcw::Operation::swap,
	     rcw::Operation::set,
	}) {
		for (const bool soft : {false, true})
			scalar_fault(harness, operation, soft, 2,
				atomic::data_address + 1U, &unchanged,
				"scalar RCW transfer alignment fault");
		scalar_fault(harness, operation, false, 31,
			atomic::data_address + 8U, &unchanged,
			"scalar RCW SP alignment fault");
		scalar_fault(harness, operation, false, 2,
			atomic::data_page + 3U * DARWIN_UML_GUEST_PAGE_SIZE,
			nullptr, "scalar RCW unmapped write fault");
		for (const bool soft : {false, true}) {
			scalar_fault(harness, operation, soft, 2,
				atomic::data_address, &read_only,
				"scalar RCW read-only atomic fault");
			scalar_fault(harness, operation, soft, 2,
				atomic::data_address, &inaccessible,
				"scalar RCW inaccessible atomic fault");
		}
	}

	/* Failed comparisons and software checks use the implementation's
	 * architecture-permitted writeback of the observed value. Consequently a
	 * read-only location still faults as a write and commits no register/flag.
	 */
	auto compare_fail = scalar_initial(rcw::Operation::compare_swap, 2,
		atomic::data_address, rcw::observed_value ^ 2U,
		rcw::operand_value, 100);
	harness.check({
		.name = "scalar RCW failed compare write-permission fault",
		.instruction = rcw::scalar_compare_swap(false, true, true,
			2, 4, 3),
		.initial = compare_fail,
		.expected = compare_fail,
		.event = engine::ExpectedEvent::memory_fault,
		.native = rcw::scalar_native(),
		.fault = {
			.address = atomic::data_address,
			.access = rcw::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = std::span<const engine::MemoryRegion>{&read_only, 1},
	});
	auto check_fail = scalar_initial(rcw::Operation::swap, 2,
		atomic::data_address, rcw::observed_value,
		rcw::operand_value | 1U, 101);
	harness.check({
		.name = "scalar RCW failed software check write-permission fault",
		.instruction = rcw::scalar_instruction(rcw::Operation::swap,
			true, true, true, 2, 3, 4),
		.initial = check_fail,
		.expected = check_fail,
		.event = engine::ExpectedEvent::memory_fault,
		.native = rcw::scalar_native(),
		.fault = {
			.address = atomic::data_address,
			.access = rcw::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = std::span<const engine::MemoryRegion>{&read_only, 1},
	});
}

void pair_alignment_mapping_and_permissions(engine::EngineHarness &harness)
{
	atomic::Page page{};
	rcw::put_pair(page, atomic::data_offset, observed_pair);
	const auto unchanged = engine::MemoryRegion{
		.address = atomic::data_page,
		.initial = page,
		.expected = page,
	};
	const auto read_only = engine::MemoryRegion{
		.address = atomic::data_page,
		.initial = page,
		.expected = page,
		.readable = true,
		.writable = false,
	};
	const auto inaccessible = engine::MemoryRegion{
		.address = atomic::data_page,
		.initial = page,
		.expected = page,
		.readable = false,
		.writable = false,
	};
	for (const auto operation : {
	     rcw::Operation::compare_swap,
	     rcw::Operation::clear,
	     rcw::Operation::swap,
	     rcw::Operation::set,
	}) {
		for (const bool soft : {false, true})
			pair_fault(harness, operation, soft, 6,
				atomic::data_address + 8U, &unchanged,
				"pair RCW transfer alignment fault");
		pair_fault(harness, operation, false, 31,
			atomic::data_address + 8U, &unchanged,
			"pair RCW SP alignment fault");
		pair_fault(harness, operation, false, 6,
			atomic::data_page + 3U * DARWIN_UML_GUEST_PAGE_SIZE,
			nullptr, "pair RCW unmapped write fault");
		for (const bool soft : {false, true}) {
			pair_fault(harness, operation, soft, 6,
				atomic::data_address, &read_only,
				"pair RCW read-only atomic fault");
			pair_fault(harness, operation, soft, 6,
				atomic::data_address, &inaccessible,
				"pair RCW inaccessible atomic fault");
		}
	}

	auto compare_fail = pair_initial(rcw::Operation::compare_swap, 6,
		atomic::data_address,
		rcw::Pair{observed_pair[0], observed_pair[1] ^ 2U},
		operand_pair, 110);
	harness.check({
		.name = "pair RCW failed compare write-permission fault",
		.instruction = rcw::pair_compare_swap(false, true, true,
			6, 4, 2),
		.initial = compare_fail,
		.expected = compare_fail,
		.event = engine::ExpectedEvent::memory_fault,
		.native = rcw::pair_native(),
		.fault = {
			.address = atomic::data_address,
			.access = rcw::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = std::span<const engine::MemoryRegion>{&read_only, 1},
	});
	auto check_fail = pair_initial(rcw::Operation::swap, 6,
		atomic::data_address, observed_pair,
		rcw::Pair{operand_pair[0] | 1U, operand_pair[1]}, 111);
	harness.check({
		.name = "pair RCW failed software check write-permission fault",
		.instruction = rcw::pair_instruction(rcw::Operation::swap,
			true, true, true, 6, 2, 4),
		.initial = check_fail,
		.expected = check_fail,
		.event = engine::ExpectedEvent::memory_fault,
		.native = rcw::pair_native(),
		.fault = {
			.address = atomic::data_address,
			.access = rcw::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = std::span<const engine::MemoryRegion>{&read_only, 1},
	});
}

void scalar_failed_update_invalidates_reservation(
	engine::EngineHarness &harness, bool software_failure)
{
	atomic::Page page{};
	const auto old = software_failure ?
		(rcw::observed_value | 1U) : rcw::observed_value;
	atomic::put(page, atomic::data_offset, old, 8);
	const auto memory = atomic::region(page, page);
	const std::array code{
		engine::ProgramWord{0, atomic::scalar_exclusive(
			3, true, false, 2, 5)},
		engine::ProgramWord{4, rcw::scalar_compare_swap(
			software_failure, false, false, 2, 4, 3)},
		engine::ProgramWord{8, atomic::scalar_exclusive(
			3, false, false, 2, 6, 7)},
		engine::ProgramWord{12, atomic::terminal_system_call},
	};
	auto initial = atomic::state(130 + static_cast<unsigned>(software_failure));
	initial.registers[2] = atomic::data_address;
	initial.registers[3] = software_failure ?
		(old & ~std::uint64_t{1}) : rcw::operand_value;
	initial.registers[4] = software_failure ? old : old ^ 2U;
	initial.registers[6] = ~old;
	auto expected = initial;
	expected.registers[5] = old;
	expected.registers[4] = old;
	expected.registers[7] = 1;
	expected.nzcv = software_failure ? 0U : rcw::compare_fail_flags;
	atomic::clear_monitor(expected);
	expected.pc = engine::engine_test_code_base + 16;
	harness.check_program({
		.name = software_failure ?
			"scalar RCW software failure invalidates reservation" :
			"scalar RCW compare failure invalidates reservation",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = rcw::scalar_native(),
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
}

void pair_failed_update_invalidates_reservation(
	engine::EngineHarness &harness, bool software_failure)
{
	auto old = observed_pair;
	if (software_failure)
		old[0] |= 1U;
	atomic::Page page{};
	rcw::put_pair(page, atomic::data_offset, old);
	const auto memory = atomic::region(page, page);
	const std::array code{
		engine::ProgramWord{0, atomic::scalar_exclusive(
			3, true, false, 6, 9)},
		engine::ProgramWord{4, rcw::pair_compare_swap(
			software_failure, false, false, 6, 4, 2)},
		engine::ProgramWord{8, atomic::scalar_exclusive(
			3, false, false, 6, 10, 11)},
		engine::ProgramWord{12, atomic::terminal_system_call},
	};
	auto initial = atomic::state(140 + static_cast<unsigned>(software_failure));
	initial.registers[6] = atomic::data_address;
	rcw::write_pair(initial, 2, software_failure ?
		rcw::Pair{old[0] & ~std::uint64_t{1}, old[1]} : operand_pair);
	rcw::write_pair(initial, 4, software_failure ?
		old : rcw::Pair{old[0], old[1] ^ 2U});
	initial.registers[10] = ~old[0];
	auto expected = initial;
	expected.registers[9] = old[0];
	rcw::write_pair(expected, 4, old);
	expected.registers[11] = 1;
	expected.nzcv = software_failure ? 0U : rcw::compare_fail_flags;
	atomic::clear_monitor(expected);
	expected.pc = engine::engine_test_code_base + 16;
	harness.check_program({
		.name = software_failure ?
			"pair RCW software failure invalidates reservation" :
			"pair RCW compare failure invalidates reservation",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = rcw::pair_native(),
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	scalar_alignment_mapping_and_permissions(harness);
	pair_alignment_mapping_and_permissions(harness);
	for (const bool software_failure : {false, true}) {
		scalar_failed_update_invalidates_reservation(
			harness, software_failure);
		pair_failed_update_invalidates_reservation(
			harness, software_failure);
	}
	harness.print_summary("arm64-base-rcw-fault");
}
