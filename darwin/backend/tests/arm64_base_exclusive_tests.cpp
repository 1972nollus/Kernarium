// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace atomic = arm64_atomic_engine_test;
namespace arm64 = uml::backend::arm64;
namespace engine = uml::backend::tests;

namespace {

constexpr std::uint64_t initial_value = 0xf1e2'd3c4'b5a6'9788ULL;
constexpr std::uint64_t stored_value = 0x1021'3243'5465'7687ULL;

void check_load(engine::EngineHarness &harness, std::uint32_t instruction,
	std::size_t size, unsigned base, unsigned destination,
	engine::NativeRequirements native)
{
	atomic::Page initial_page{};
	atomic::put(initial_page, atomic::data_offset, initial_value, 8);
	const auto expected_page = initial_page;
	const auto memory = atomic::region(initial_page, expected_page);
	const std::array code{
		engine::ProgramWord{0, instruction},
		engine::ProgramWord{4, atomic::clrex(7)},
		engine::ProgramWord{8, atomic::terminal_system_call},
	};
	auto initial = atomic::state(1);
	if (base == 31U)
		initial.sp = atomic::data_address;
	else
		initial.registers[base] = atomic::data_address;
	auto expected = initial;
	atomic::write_register(expected, destination,
		initial_value & atomic::mask_for_size(size));
	atomic::clear_monitor(expected);
	expected.pc = engine::engine_test_code_base + 12;
	harness.check_program({
		.name = "scalar exclusive load",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = native,
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
}

void check_failed_store(engine::EngineHarness &harness,
	std::uint32_t instruction, unsigned base, unsigned source,
	unsigned status, engine::NativeRequirements native)
{
	atomic::Page page{};
	atomic::put(page, atomic::data_offset, initial_value, 8);
	const auto memory = atomic::region(page, page);
	auto initial = atomic::state(2);
	if (base == 31U)
		initial.sp = atomic::data_address;
	else
		initial.registers[base] = atomic::data_address;
	atomic::write_register(initial, source, stored_value);
	auto expected = initial;
	atomic::write_register(expected, status, 1);
	harness.check({
		.name = "scalar exclusive store without monitor",
		.instruction = instruction,
		.initial = initial,
		.expected = expected,
		.native = native,
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
}

void check_success(engine::EngineHarness &harness,
	std::uint32_t load_instruction, std::uint32_t store_instruction,
	std::size_t size, engine::NativeRequirements native)
{
	atomic::Page initial_page{};
	atomic::put(initial_page, atomic::data_offset, initial_value, 8);
	auto expected_page = initial_page;
	atomic::put(expected_page, atomic::data_offset, stored_value, size);
	const auto memory = atomic::region(initial_page, expected_page);
	const std::array code{
		engine::ProgramWord{0, atomic::store_x(28, 26)},
		engine::ProgramWord{4, atomic::load_x(28, 27)},
		engine::ProgramWord{8, load_instruction},
		engine::ProgramWord{12, store_instruction},
		engine::ProgramWord{16, atomic::cbnz_w(5, -8)},
		engine::ProgramWord{20, atomic::clrex(11)},
		engine::ProgramWord{24, atomic::terminal_system_call},
	};
	auto initial = atomic::state(3);
	initial.registers[28] = atomic::data_address;
	initial.registers[26] = initial_value;
	initial.registers[2] = atomic::data_address;
	initial.registers[3] = ~std::uint64_t{0};
	initial.registers[4] = stored_value;
	initial.registers[5] = ~std::uint64_t{0};
	auto expected = initial;
	expected.registers[27] = initial_value;
	expected.registers[3] = initial_value & atomic::mask_for_size(size);
	expected.registers[5] = 0;
	atomic::clear_monitor(expected);
	expected.pc = engine::engine_test_code_base + 28;
	harness.check_program({
		.name = "scalar exclusive successful sequence",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = native,
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
		.expected_instruction_pattern = atomic::exclusive_retry_count,
	});
}

void standard_variants(engine::EngineHarness &harness)
{
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		for (const bool load_ordered : {false, true}) {
			for (const unsigned base : {2U, 31U})
				for (const unsigned destination : {0U, 30U, 31U})
					check_load(harness, atomic::scalar_exclusive(size_code,
						true, load_ordered, base, destination),
						size, base, destination, atomic::baseline());
			for (const bool store_ordered : {false, true}) {
				check_success(harness,
					atomic::scalar_exclusive(size_code, true,
						load_ordered, 2, 3),
					atomic::scalar_exclusive(size_code, false,
						store_ordered, 2, 4, 5),
					size, atomic::baseline());
			}
		}
		for (const bool store_ordered : {false, true})
			for (const unsigned base : {2U, 31U})
				for (const unsigned source : {4U, 31U})
					for (const unsigned status : {5U, 31U})
						check_failed_store(harness,
							atomic::scalar_exclusive(size_code, false,
								store_ordered, base, source, status),
							base, source, status, atomic::baseline());
	}

	for (unsigned destination = 0; destination < 32; ++destination)
		check_load(harness,
			atomic::scalar_exclusive(3, true, false, 2, destination),
			8, 2, destination, atomic::baseline());
	for (unsigned base = 0; base < 32; ++base)
		check_load(harness,
			atomic::scalar_exclusive(3, true, true, base, 3),
			8, base, 3, atomic::baseline());
}

void unprivileged_variants(engine::EngineHarness &harness)
{
	for (const bool wide : {false, true}) {
		const auto size = wide ? 8U : 4U;
		for (const bool load_ordered : {false, true}) {
			for (const unsigned base : {2U, 31U})
				for (const unsigned destination : {3U, 31U})
					check_load(harness,
						atomic::unprivileged_exclusive(wide, true,
							load_ordered, base, destination),
						size, base, destination, atomic::lsui());
			for (const bool store_ordered : {false, true})
				check_success(harness,
					atomic::unprivileged_exclusive(wide, true,
						load_ordered, 2, 3),
					atomic::unprivileged_exclusive(wide, false,
						store_ordered, 2, 4, 5),
					size, atomic::lsui());
		}
		for (const bool store_ordered : {false, true})
			for (const unsigned source : {4U, 31U})
				for (const unsigned status : {5U, 31U})
					check_failed_store(harness,
						atomic::unprivileged_exclusive(wide, false,
							store_ordered, 2, source, status),
						2, source, status, atomic::lsui());
	}
}

void reservation_invalidation(engine::EngineHarness &harness)
{
	atomic::Page page{};
	atomic::put(page, atomic::data_offset, initial_value, 8);
	const auto memory = atomic::region(page, page);
	const std::array code{
		engine::ProgramWord{0, atomic::store_x(28, 26)},
		engine::ProgramWord{4, atomic::load_x(28, 27)},
		engine::ProgramWord{8,
			atomic::scalar_exclusive(3, true, false, 2, 3)},
		engine::ProgramWord{12, atomic::store_x(28, 26)},
		engine::ProgramWord{16,
			atomic::scalar_exclusive(3, false, false, 2, 4, 5)},
		engine::ProgramWord{20, atomic::clrex(4)},
		engine::ProgramWord{24, atomic::terminal_system_call},
	};
	auto initial = atomic::state(8);
	initial.registers[28] = atomic::data_address;
	initial.registers[26] = initial_value;
	initial.registers[2] = atomic::data_address;
	initial.registers[4] = stored_value;
	initial.registers[5] = ~std::uint64_t{0};
	auto expected = initial;
	expected.registers[27] = initial_value;
	expected.registers[3] = initial_value;
	expected.registers[5] = 1;
	atomic::clear_monitor(expected);
	expected.pc = engine::engine_test_code_base + 28;
	harness.check_program({
		.name = "scalar exclusive reservation invalidation",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = atomic::baseline(),
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
}

void faults(engine::EngineHarness &harness)
{
	atomic::Page page{};
	atomic::put(page, atomic::data_offset, initial_value, 8);
	const auto mapped = atomic::region(page, page);
	const std::span<const engine::MemoryRegion> mapping{&mapped, 1};
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		for (const bool load : {false, true}) {
			for (const bool ordered : {false, true}) {
				auto initial = atomic::state(4);
				initial.sp = atomic::data_address + 8;
				initial.registers[3] = stored_value;
				const auto instruction = atomic::scalar_exclusive(size_code,
					load, ordered, 31, 3, load ? 31U : 5U);
				harness.check({
					.name = "scalar exclusive SP alignment",
					.instruction = instruction,
					.initial = initial,
					.expected = initial,
					.event = engine::ExpectedEvent::memory_fault,
					.native = atomic::baseline(),
					.fault = {
						.address = atomic::data_address + 8,
						.access = load ? arm64::MemoryAccess::read :
							arm64::MemoryAccess::write,
					},
					.compare_fault = true,
					.memory = mapping,
				});
			}
		}
		if (size_code != 0U) {
			for (const bool load : {false, true}) {
				auto initial = atomic::state(5);
				initial.registers[2] = atomic::data_address + 1;
				initial.registers[3] = stored_value;
				harness.check({
					.name = "scalar exclusive natural alignment",
					.instruction = atomic::scalar_exclusive(size_code,
						load, true, 2, 3, load ? 31U : 5U),
					.initial = initial,
					.expected = initial,
					.event = engine::ExpectedEvent::memory_fault,
					.native = atomic::baseline(),
					.fault = {
						.address = atomic::data_address + 1,
						.access = load ? arm64::MemoryAccess::read :
							arm64::MemoryAccess::write,
					},
					.compare_fault = true,
					.memory = mapping,
				});
			}
		}
	}

	for (const bool load : {false, true}) {
		auto initial = atomic::state(6);
		initial.registers[2] = atomic::data_page +
			DARWIN_UML_GUEST_PAGE_SIZE * 4ULL;
		harness.check({
			.name = "scalar exclusive unmapped",
			.instruction = atomic::scalar_exclusive(3, load, true, 2, 3,
				load ? 31U : 5U),
			.initial = initial,
			.expected = initial,
			.event = engine::ExpectedEvent::memory_fault,
			.native = atomic::baseline(),
			.fault = {
				.address = initial.registers[2],
				.access = load ? arm64::MemoryAccess::read :
					arm64::MemoryAccess::write,
			},
			.compare_fault = true,
		});
	}
}

void allocation(engine::EngineHarness &harness)
{
	auto initial = atomic::state(7);
	initial.registers[2] = atomic::data_address;
	for (const unsigned status : {0U, 1U, 15U, 30U})
		harness.check_illegal("scalar exclusive reserved load status",
			atomic::scalar_exclusive(3, true, false, 2, 3,
				status, 31),
			initial, atomic::baseline());
	for (const unsigned rt2 : {0U, 1U, 15U, 30U})
		harness.check_illegal("scalar exclusive reserved load Rt2",
			atomic::scalar_exclusive(3, true, false, 2, 3,
				31, rt2),
			initial, atomic::baseline());
	for (const unsigned rt2 : {0U, 1U, 15U, 30U})
		harness.check_illegal("scalar exclusive reserved store",
			atomic::scalar_exclusive(3, false, false, 2, 3, 5, rt2),
			initial, atomic::baseline());
	for (const unsigned status : {0U, 1U, 15U, 30U})
		harness.check_illegal(
			"unprivileged exclusive reserved load status",
			atomic::unprivileged_exclusive(true, true, false, 2, 3,
				status, 31),
			initial, atomic::lsui());
	for (const unsigned rt2 : {0U, 1U, 15U, 30U})
		harness.check_illegal("unprivileged exclusive reserved load Rt2",
			atomic::unprivileged_exclusive(true, true, false, 2, 3,
				31, rt2),
			initial, atomic::lsui());
	for (const unsigned rt2 : {0U, 1U, 15U, 30U})
		harness.check_illegal("unprivileged exclusive reserved store Rt2",
			atomic::unprivileged_exclusive(true, false, false, 2, 3,
				5, rt2),
			initial, atomic::lsui());
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	standard_variants(harness);
	unprivileged_variants(harness);
	reservation_invalidation(harness);
	faults(harness);
	allocation(harness);
	harness.print_summary("arm64-base-exclusive");
	return 0;
}
