// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace atomic = arm64_atomic_engine_test;
namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;

namespace {

constexpr std::array observed_values{
	std::uint64_t{0xf1e2'd3c4'b5a6'9788},
	std::uint64_t{0x8071'6253'4435'2617},
};
constexpr std::array replacement_values{
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

void check_case(engine::EngineHarness &harness, std::uint32_t instruction,
	std::size_t element_size, unsigned base, unsigned compare,
	unsigned replacement, bool succeeds,
	engine::NativeRequirements native)
{
	const auto mask = atomic::mask_for_size(element_size);
	atomic::Page initial_page{};
	put_pair(initial_page, observed_values, element_size);
	auto expected_page = initial_page;
	auto initial = atomic::state(30);
	atomic::write_register(initial, compare,
		succeeds ? observed_values[0] : (observed_values[0] ^ 1U));
	atomic::write_register(initial, compare + 1U, observed_values[1]);
	atomic::write_register(initial, replacement, replacement_values[0]);
	atomic::write_register(initial, replacement + 1U, replacement_values[1]);
	/* Resolve a base/input alias as one architectural register value. */
	atomic::write_register(initial, base, atomic::data_address, true);
	const auto compare_first = atomic::read_register(initial, compare) & mask;
	const auto compare_second =
		atomic::read_register(initial, compare + 1U) & mask;
	const auto replacement_first =
		atomic::read_register(initial, replacement) & mask;
	const auto replacement_second =
		atomic::read_register(initial, replacement + 1U) & mask;
	const bool actual_success =
		compare_first == (observed_values[0] & mask) &&
		compare_second == (observed_values[1] & mask);
	if (actual_success) {
		atomic::put(expected_page, atomic::data_offset,
			replacement_first, element_size);
		atomic::put(expected_page, atomic::data_offset + element_size,
			replacement_second, element_size);
	}
	auto expected = initial;
	atomic::write_register(expected, compare, observed_values[0] & mask);
	atomic::write_register(expected, compare + 1U,
		observed_values[1] & mask);
	const auto memory = atomic::region(initial_page, expected_page);
	harness.check({
		.name = "pair compare-and-swap behavior",
		.instruction = instruction,
		.initial = initial,
		.expected = expected,
		.native = native,
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
}

void standard_variants(engine::EngineHarness &harness)
{
	for (const bool wide : {false, true}) {
		const auto element_size = wide ? 8U : 4U;
		for (const bool acquire : {false, true})
			for (const bool release : {false, true})
				for (const bool succeeds : {false, true})
					for (const auto registers : {
						std::array{2U, 4U, 6U},
						std::array{31U, 4U, 6U},
						std::array{2U, 30U, 6U},
						std::array{2U, 4U, 30U},
						std::array{2U, 4U, 4U},
						std::array{4U, 4U, 6U},
					})
						check_case(harness,
							atomic::compare_swap_pair(wide, acquire,
								release, registers[0], registers[1],
								registers[2]),
							element_size, registers[0], registers[1],
							registers[2], succeeds, atomic::lse());
	}

	for (unsigned compare = 0; compare <= 30; compare += 2)
		check_case(harness,
			atomic::compare_swap_pair(true, true, false, 2, compare, 6),
			8, 2, compare, 6, false, atomic::lse());
	for (unsigned replacement = 0; replacement <= 30; replacement += 2)
		check_case(harness,
			atomic::compare_swap_pair(false, false, true, 2, 4,
				replacement),
			4, 2, 4, replacement, true, atomic::lse());
	for (unsigned base = 0; base < 32; ++base)
		check_case(harness,
			atomic::compare_swap_pair(true, true, true, base, 4, 6),
			8, base, 4, 6, true, atomic::lse());
}

void unprivileged_variants(engine::EngineHarness &harness)
{
	for (const bool acquire : {false, true})
		for (const bool release : {false, true})
			for (const bool succeeds : {false, true})
				for (const auto registers : {
					std::array{2U, 4U, 6U},
					std::array{31U, 4U, 6U},
					std::array{2U, 30U, 6U},
					std::array{2U, 4U, 30U},
					std::array{2U, 4U, 4U},
				})
					check_case(harness,
						atomic::unprivileged_compare_swap_pair(true,
							acquire, release, registers[0], registers[1],
							registers[2]),
						8, registers[0], registers[1], registers[2],
						succeeds, atomic::lsui());
}

void invalidates_exclusive_monitor(engine::EngineHarness &harness)
{
	atomic::Page page{};
	put_pair(page, observed_values, 8);
	const auto memory = atomic::region(page, page);
	const std::array code{
		engine::ProgramWord{0, atomic::store_x(28, 26)},
		engine::ProgramWord{4, atomic::load_x(28, 27)},
		engine::ProgramWord{8, atomic::pair_exclusive(true, true, false,
			2, 8, 9)},
		engine::ProgramWord{12, atomic::compare_swap_pair(true, false,
			false, 2, 4, 6)},
		engine::ProgramWord{16, atomic::pair_exclusive(true, false, false,
			2, 6, 7, 10)},
		engine::ProgramWord{20, atomic::clrex(4)},
		engine::ProgramWord{24, atomic::terminal_system_call},
	};
	auto initial = atomic::state(31);
	initial.registers[28] = atomic::data_address;
	initial.registers[26] = observed_values[0];
	initial.registers[2] = atomic::data_address;
	initial.registers[4] = observed_values[0] ^ 1U;
	initial.registers[5] = observed_values[1];
	initial.registers[6] = replacement_values[0];
	initial.registers[7] = replacement_values[1];
	initial.registers[10] = ~std::uint64_t{0};
	auto expected = initial;
	expected.registers[27] = observed_values[0];
	expected.registers[8] = observed_values[0];
	expected.registers[9] = observed_values[1];
	expected.registers[4] = observed_values[0];
	expected.registers[5] = observed_values[1];
	expected.registers[10] = 1;
	atomic::clear_monitor(expected);
	expected.pc = engine::engine_test_code_base + 28;
	harness.check_program({
		.name = "pair compare-and-swap invalidates exclusive monitor",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = atomic::lse(),
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
}

void faults(engine::EngineHarness &harness)
{
	atomic::Page page{};
	put_pair(page, observed_values, 8);
	const auto unchanged = atomic::region(page, page);
	for (const bool wide : {false, true}) {
		const auto element_size = wide ? 8U : 4U;
		auto initial = atomic::state(32);
		initial.sp = atomic::data_address + 8;
		initial.registers[4] = observed_values[0];
		initial.registers[5] = observed_values[1];
		initial.registers[6] = replacement_values[0];
		initial.registers[7] = replacement_values[1];
		harness.check({
			.name = "pair compare-and-swap SP alignment",
			.instruction = atomic::compare_swap_pair(wide, true, true,
				31, 4, 6),
			.initial = initial,
			.expected = initial,
			.event = engine::ExpectedEvent::memory_fault,
			.native = atomic::lse(),
			.fault = {
				.address = atomic::data_address + 8,
				.access = arm64::MemoryAccess::write,
			},
			.compare_fault = true,
			.memory = std::span<const engine::MemoryRegion>{&unchanged, 1},
		});

		initial = atomic::state(33);
		initial.registers[2] = atomic::data_address + element_size;
		initial.registers[4] = observed_values[0];
		initial.registers[5] = observed_values[1];
		initial.registers[6] = replacement_values[0];
		initial.registers[7] = replacement_values[1];
		harness.check({
			.name = "pair compare-and-swap transfer alignment",
			.instruction = atomic::compare_swap_pair(wide, true, true,
				2, 4, 6),
			.initial = initial,
			.expected = initial,
			.event = engine::ExpectedEvent::memory_fault,
			.native = atomic::lse(),
			.fault = {
				.address = atomic::data_address + element_size,
				.access = arm64::MemoryAccess::write,
			},
			.compare_fault = true,
			.memory = std::span<const engine::MemoryRegion>{&unchanged, 1},
		});
	}

	for (const bool compare_matches : {false, true}) {
		auto initial = atomic::state(34);
		initial.registers[2] = atomic::data_address;
		initial.registers[4] = compare_matches ? observed_values[0] :
			(observed_values[0] ^ 1U);
		initial.registers[5] = observed_values[1];
		initial.registers[6] = replacement_values[0];
		initial.registers[7] = replacement_values[1];
		const auto read_only = atomic::region(page, page, true, false);
		harness.check({
			.name = "pair compare-and-swap write permission",
			.instruction = atomic::compare_swap_pair(true, true, true,
				2, 4, 6),
			.initial = initial,
			.expected = initial,
			.event = engine::ExpectedEvent::memory_fault,
			.native = atomic::lse(),
			.fault = {
				.address = atomic::data_address,
				.access = arm64::MemoryAccess::write,
			},
			.compare_fault = true,
			.memory = std::span<const engine::MemoryRegion>{&read_only, 1},
		});
	}
}

void allocation(engine::EngineHarness &harness)
{
	auto initial = atomic::state(35);
	initial.registers[2] = atomic::data_address;
	for (const bool wide : {false, true})
		for (const bool acquire : {false, true})
			for (const bool release : {false, true}) {
				for (const unsigned odd : {1U, 3U, 15U, 29U, 31U}) {
					harness.check_illegal(
						"pair compare-and-swap odd compare",
						atomic::compare_swap_pair(wide, acquire, release,
							2, odd, 6),
						initial, atomic::lse());
					harness.check_illegal(
						"pair compare-and-swap odd replacement",
						atomic::compare_swap_pair(wide, acquire, release,
							2, 4, odd),
						initial, atomic::lse());
				}
				for (const unsigned rt2 : {0U, 1U, 15U, 30U})
					harness.check_illegal(
						"pair compare-and-swap reserved Rt2",
						atomic::compare_swap_pair(wide, acquire, release,
							2, 4, 6, rt2),
						initial, atomic::lse());
			}
	for (const bool acquire : {false, true})
		for (const bool release : {false, true}) {
			harness.check_illegal(
				"unprivileged pair compare-and-swap reserved width",
				atomic::unprivileged_compare_swap_pair(false, acquire,
					release, 2, 4, 6),
				initial, atomic::lsui());
			for (const unsigned odd : {1U, 3U, 15U, 29U, 31U}) {
				harness.check_illegal(
					"unprivileged pair compare-and-swap odd compare",
					atomic::unprivileged_compare_swap_pair(true, acquire,
						release, 2, odd, 6),
					initial, atomic::lsui());
				harness.check_illegal(
					"unprivileged pair compare-and-swap odd replacement",
					atomic::unprivileged_compare_swap_pair(true, acquire,
						release, 2, 4, odd),
					initial, atomic::lsui());
			}
		}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	standard_variants(harness);
	unprivileged_variants(harness);
	invalidates_exclusive_monitor(harness);
	faults(harness);
	allocation(harness);
	harness.print_summary("arm64-base-compare-swap-pair");
	return 0;
}
