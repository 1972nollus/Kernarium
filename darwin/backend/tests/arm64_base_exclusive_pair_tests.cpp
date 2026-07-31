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

constexpr std::array initial_values{
	std::uint64_t{0xf1e2'd3c4'b5a6'9788},
	std::uint64_t{0x8071'6253'4435'2617},
};
constexpr std::array stored_values{
	std::uint64_t{0x1021'3243'5465'7687},
	std::uint64_t{0x899a'abbc'cdde'eff0},
};

void put_pair(atomic::Page &page, const std::array<std::uint64_t, 2> &values,
	std::size_t element_size)
{
	atomic::put(page, atomic::data_offset, values[0], element_size);
	atomic::put(page, atomic::data_offset + element_size, values[1],
		element_size);
}

void check_load(engine::EngineHarness &harness, bool wide, bool ordered,
	unsigned base, unsigned first, unsigned second)
{
	const auto element_size = wide ? 8U : 4U;
	atomic::Page page{};
	put_pair(page, initial_values, element_size);
	const auto memory = atomic::region(page, page);
	const std::array code{
		engine::ProgramWord{0, atomic::pair_exclusive(wide, true, ordered,
			base, first, second)},
		engine::ProgramWord{4, atomic::clrex(3)},
		engine::ProgramWord{8, atomic::terminal_system_call},
	};
	auto initial = atomic::state(10);
	atomic::write_register(initial, base, atomic::data_address, true);
	auto expected = initial;
	const auto mask = atomic::mask_for_size(element_size);
	atomic::write_register(expected, first, initial_values[0] & mask);
	atomic::write_register(expected, second, initial_values[1] & mask);
	atomic::clear_monitor(expected);
	expected.pc = engine::engine_test_code_base + 12;
	harness.check_program({
		.name = "pair exclusive load",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = atomic::baseline(),
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
}

void check_failed_store(engine::EngineHarness &harness, bool wide,
	bool ordered, unsigned base, unsigned first, unsigned second,
	unsigned status)
{
	const auto element_size = wide ? 8U : 4U;
	atomic::Page page{};
	put_pair(page, initial_values, element_size);
	const auto memory = atomic::region(page, page);
	auto initial = atomic::state(11);
	atomic::write_register(initial, base, atomic::data_address, true);
	atomic::write_register(initial, first, stored_values[0]);
	atomic::write_register(initial, second, stored_values[1]);
	auto expected = initial;
	atomic::write_register(expected, status, 1);
	harness.check({
		.name = "pair exclusive store without monitor",
		.instruction = atomic::pair_exclusive(wide, false, ordered, base,
			first, second, status),
		.initial = initial,
		.expected = expected,
		.native = atomic::baseline(),
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
}

void check_success(engine::EngineHarness &harness, bool wide,
	bool load_ordered, bool store_ordered)
{
	const auto element_size = wide ? 8U : 4U;
	atomic::Page initial_page{};
	put_pair(initial_page, initial_values, element_size);
	auto expected_page = initial_page;
	put_pair(expected_page, stored_values, element_size);
	const auto memory = atomic::region(initial_page, expected_page);
	const std::array code{
		engine::ProgramWord{0, atomic::store_x(28, 26)},
		engine::ProgramWord{4, atomic::load_x(28, 27)},
		engine::ProgramWord{8, atomic::pair_exclusive(wide, true,
			load_ordered, 2, 3, 4)},
		engine::ProgramWord{12, atomic::pair_exclusive(wide, false,
			store_ordered, 2, 5, 6, 7)},
		engine::ProgramWord{16, atomic::cbnz_w(7, -8)},
		engine::ProgramWord{20, atomic::clrex(9)},
		engine::ProgramWord{24, atomic::terminal_system_call},
	};
	auto initial = atomic::state(12);
	initial.registers[28] = atomic::data_address;
	initial.registers[26] = atomic::get(initial_page, atomic::data_offset, 8);
	initial.registers[2] = atomic::data_address;
	initial.registers[5] = stored_values[0];
	initial.registers[6] = stored_values[1];
	initial.registers[7] = ~std::uint64_t{0};
	auto expected = initial;
	expected.registers[27] = initial.registers[26];
	const auto mask = atomic::mask_for_size(element_size);
	expected.registers[3] = initial_values[0] & mask;
	expected.registers[4] = initial_values[1] & mask;
	expected.registers[7] = 0;
	atomic::clear_monitor(expected);
	expected.pc = engine::engine_test_code_base + 28;
	harness.check_program({
		.name = "pair exclusive successful sequence",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = atomic::baseline(),
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
		.expected_instruction_pattern = atomic::exclusive_retry_count,
	});
}

void variants(engine::EngineHarness &harness)
{
	for (const bool wide : {false, true}) {
		for (const bool load_ordered : {false, true}) {
			for (const unsigned base : {2U, 31U})
				for (const auto pair : {
					std::array{3U, 4U},
					std::array{31U, 4U},
					std::array{3U, 31U},
				})
					check_load(harness, wide, load_ordered, base,
						pair[0], pair[1]);
			for (const bool store_ordered : {false, true})
				check_success(harness, wide, load_ordered, store_ordered);
		}
		for (const bool store_ordered : {false, true})
			for (const auto registers : {
				std::array{2U, 5U, 6U, 7U},
				std::array{31U, 31U, 6U, 7U},
				std::array{2U, 5U, 31U, 7U},
				std::array{2U, 5U, 6U, 31U},
			})
				check_failed_store(harness, wide, store_ordered,
					registers[0], registers[1], registers[2],
					registers[3]);
	}

	for (unsigned first = 0; first < 32; ++first) {
		const unsigned second = first == 4U ? 5U : 4U;
		check_load(harness, true, false, 2, first, second);
	}
	for (unsigned second = 0; second < 32; ++second) {
		const unsigned first = second == 3U ? 4U : 3U;
		check_load(harness, false, true, 2, first, second);
	}
	for (unsigned base = 0; base < 32; ++base)
		check_load(harness, true, true, base, 3, 4);
}

void cross_shape_monitor(engine::EngineHarness &harness)
{
	atomic::Page initial_page{};
	put_pair(initial_page, initial_values, 4);
	auto expected_page = initial_page;
	const auto packed = (stored_values[0] & 0xffff'ffffU) |
		((stored_values[1] & 0xffff'ffffU) << 32);
	atomic::put(expected_page, atomic::data_offset, packed, 8);
	const auto memory = atomic::region(initial_page, expected_page);
	const std::array pair_to_scalar{
		engine::ProgramWord{0, atomic::store_x(28, 26)},
		engine::ProgramWord{4, atomic::load_x(28, 27)},
		engine::ProgramWord{8, atomic::pair_exclusive(false, true, false,
			2, 3, 4)},
		engine::ProgramWord{12, atomic::scalar_exclusive(3, false, false,
			2, 5, 7)},
		engine::ProgramWord{16, atomic::cbnz_w(7, -8)},
		engine::ProgramWord{20, atomic::clrex(1)},
		engine::ProgramWord{24, atomic::terminal_system_call},
	};
	auto initial = atomic::state(13);
	initial.registers[28] = atomic::data_address;
	initial.registers[26] = atomic::get(initial_page, atomic::data_offset, 8);
	initial.registers[2] = atomic::data_address;
	initial.registers[5] = packed;
	initial.registers[7] = ~std::uint64_t{0};
	auto expected = initial;
	expected.registers[27] = initial.registers[26];
	expected.registers[3] = initial_values[0] & 0xffff'ffffU;
	expected.registers[4] = initial_values[1] & 0xffff'ffffU;
	expected.registers[7] = 0;
	atomic::clear_monitor(expected);
	expected.pc = engine::engine_test_code_base + 28;
	harness.check_program({
		.name = "pair load to scalar store monitor range",
		.code = pair_to_scalar,
		.initial = initial,
		.expected = expected,
		.native = atomic::baseline(),
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
		.expected_instruction_pattern = atomic::exclusive_retry_count,
	});

	auto second_expected_page = initial_page;
	put_pair(second_expected_page, stored_values, 4);
	const auto second_memory =
		atomic::region(initial_page, second_expected_page);
	const std::array scalar_to_pair{
		engine::ProgramWord{0, atomic::store_x(28, 26)},
		engine::ProgramWord{4, atomic::load_x(28, 27)},
		engine::ProgramWord{8, atomic::scalar_exclusive(3, true, false,
			2, 3)},
		engine::ProgramWord{12, atomic::pair_exclusive(false, false, false,
			2, 5, 6, 7)},
		engine::ProgramWord{16, atomic::cbnz_w(7, -8)},
		engine::ProgramWord{20, atomic::clrex(2)},
		engine::ProgramWord{24, atomic::terminal_system_call},
	};
	initial = atomic::state(14);
	initial.registers[28] = atomic::data_address;
	initial.registers[26] = atomic::get(initial_page, atomic::data_offset, 8);
	initial.registers[2] = atomic::data_address;
	initial.registers[5] = stored_values[0];
	initial.registers[6] = stored_values[1];
	initial.registers[7] = ~std::uint64_t{0};
	expected = initial;
	expected.registers[27] = initial.registers[26];
	expected.registers[3] =
		atomic::get(initial_page, atomic::data_offset, 8);
	expected.registers[7] = 0;
	atomic::clear_monitor(expected);
	expected.pc = engine::engine_test_code_base + 28;
	harness.check_program({
		.name = "scalar load to pair store monitor range",
		.code = scalar_to_pair,
		.initial = initial,
		.expected = expected,
		.native = atomic::baseline(),
		.memory = std::span<const engine::MemoryRegion>{&second_memory, 1},
		.expected_instruction_pattern = atomic::exclusive_retry_count,
	});
}

void faults(engine::EngineHarness &harness)
{
	atomic::Page page{};
	put_pair(page, initial_values, 8);
	const auto memory = atomic::region(page, page);
	for (const bool wide : {false, true}) {
		const auto element_size = wide ? 8U : 4U;
		for (const bool load : {false, true}) {
			auto initial = atomic::state(15);
			initial.sp = atomic::data_address + 8;
			initial.registers[5] = stored_values[0];
			initial.registers[6] = stored_values[1];
			harness.check({
				.name = "pair exclusive SP alignment",
				.instruction = atomic::pair_exclusive(wide, load, true,
					31, 5, 6, load ? 31U : 7U),
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
				.memory = std::span<const engine::MemoryRegion>{&memory, 1},
			});

			initial = atomic::state(16);
			initial.registers[2] = atomic::data_address + element_size;
			harness.check({
				.name = "pair exclusive transfer alignment",
				.instruction = atomic::pair_exclusive(wide, load, true,
					2, 5, 6, load ? 31U : 7U),
				.initial = initial,
				.expected = initial,
				.event = engine::ExpectedEvent::memory_fault,
				.native = atomic::baseline(),
				.fault = {
					.address = atomic::data_address + element_size,
					.access = load ? arm64::MemoryAccess::read :
						arm64::MemoryAccess::write,
				},
				.compare_fault = true,
				.memory = std::span<const engine::MemoryRegion>{&memory, 1},
			});
		}
	}
}

void allocation(engine::EngineHarness &harness)
{
	auto initial = atomic::state(17);
	initial.registers[2] = atomic::data_address;
	for (const bool wide : {false, true}) {
		for (const unsigned duplicate : {0U, 1U, 15U, 30U, 31U})
			harness.check_illegal("pair exclusive duplicate load",
				atomic::pair_exclusive(wide, true, false, 2,
					duplicate, duplicate),
				initial, atomic::baseline());
		for (const unsigned status : {0U, 1U, 15U, 30U})
			harness.check_illegal("pair exclusive reserved load status",
				atomic::pair_exclusive(wide, true, true, 2, 3, 4,
					status),
				initial, atomic::baseline());
	}
	for (const auto instruction : {
		atomic::pair_exclusive(false, true, false, 2, 3, 4) &
			0x7fff'ffffU,
		atomic::pair_exclusive(true, false, false, 2, 3, 4, 5) &
			0x7fff'ffffU,
	}) {
		harness.check_illegal("pair exclusive unallocated size",
			instruction, initial, atomic::baseline());
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	variants(harness);
	cross_shape_monitor(harness);
	faults(harness);
	allocation(harness);
	harness.print_summary("arm64-base-exclusive-pair");
	return 0;
}
