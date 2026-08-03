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

constexpr std::uint64_t observed_value = 0xf1e2'd3c4'b5a6'9788ULL;
constexpr std::uint64_t replacement_value = 0x1021'3243'5465'7687ULL;

void check_case(engine::EngineHarness &harness, std::uint32_t instruction,
	std::size_t size, unsigned base, unsigned compare, unsigned replacement,
	bool succeeds, engine::NativeRequirements native)
{
	const auto mask = atomic::mask_for_size(size);
	const auto observed = observed_value & mask;
	atomic::Page initial_page{};
	atomic::put(initial_page, atomic::data_offset, observed_value, 8);
	auto expected_page = initial_page;
	auto initial = atomic::state(20);
	const auto compare_value = succeeds ? observed :
		(observed ^ std::uint64_t{1});
	atomic::write_register(initial, compare, compare_value);
	atomic::write_register(initial, replacement, replacement_value);
	/* If the base aliases an input, the one architectural register must hold
	 * the address. The oracle below captures that final aliased input value. */
	atomic::write_register(initial, base, atomic::data_address, true);
	/*
	 * Inputs are captured before the compare/result register is written.
	 * Model aliases from the fully initialized architectural state.
	 */
	const auto captured_compare =
		atomic::read_register(initial, compare) & mask;
	const auto captured_replacement =
		atomic::read_register(initial, replacement) & mask;
	const bool actual_success = captured_compare == observed;
	if (actual_success)
		atomic::put(expected_page, atomic::data_offset,
			captured_replacement, size);
	auto expected = initial;
	atomic::write_register(expected, compare, observed);
	const auto memory = atomic::region(initial_page, expected_page);
	harness.check({
		.name = "scalar compare-and-swap behavior",
		.instruction = instruction,
		.initial = initial,
		.expected = expected,
		.native = native,
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
}

void standard_variants(engine::EngineHarness &harness)
{
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		const auto size = std::size_t{1} << size_code;
		for (const bool acquire : {false, true})
			for (const bool release : {false, true})
				for (const bool succeeds : {false, true})
					for (const auto registers : {
						std::array{2U, 3U, 4U},
						std::array{31U, 3U, 4U},
						std::array{2U, 31U, 4U},
						std::array{2U, 3U, 31U},
						std::array{2U, 3U, 3U},
						std::array{3U, 3U, 4U},
					})
						check_case(harness,
							atomic::compare_swap(size_code, acquire,
								release, registers[0], registers[1],
								registers[2]),
							size, registers[0], registers[1], registers[2],
							succeeds, atomic::lse());
	}

	for (unsigned compare = 0; compare < 32; ++compare)
		check_case(harness,
			atomic::compare_swap(3, true, false, 2, compare, 4),
			8, 2, compare, 4, false, atomic::lse());
	for (unsigned replacement = 0; replacement < 32; ++replacement)
		check_case(harness,
			atomic::compare_swap(2, false, true, 2, 3, replacement),
			4, 2, 3, replacement, true, atomic::lse());
	for (unsigned base = 0; base < 32; ++base)
		check_case(harness,
			atomic::compare_swap(3, true, true, base, 3, 4),
			8, base, 3, 4, true, atomic::lse());
}

void unprivileged_variants(engine::EngineHarness &harness)
{
	for (const bool acquire : {false, true})
		for (const bool release : {false, true})
			for (const bool succeeds : {false, true})
				for (const auto registers : {
					std::array{2U, 3U, 4U},
					std::array{31U, 3U, 4U},
					std::array{2U, 31U, 4U},
					std::array{2U, 3U, 31U},
					std::array{3U, 3U, 4U},
				})
					check_case(harness,
						atomic::unprivileged_compare_swap(true, acquire,
							release, registers[0], registers[1],
							registers[2]),
						8, registers[0], registers[1], registers[2],
						succeeds, atomic::lsui());
}

void faults(engine::EngineHarness &harness)
{
	atomic::Page page{};
	atomic::put(page, atomic::data_offset, observed_value, 8);
	const auto unchanged = atomic::region(page, page);
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		auto initial = atomic::state(21);
		initial.sp = atomic::data_address + 8;
		initial.registers[3] = observed_value;
		initial.registers[4] = replacement_value;
		harness.check({
			.name = "scalar compare-and-swap SP alignment",
			.instruction = atomic::compare_swap(size_code, true, true,
				31, 3, 4),
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
		if (size_code != 0U) {
			initial = atomic::state(22);
			initial.registers[2] = atomic::data_address + 1;
			initial.registers[3] = observed_value;
			initial.registers[4] = replacement_value;
			harness.check({
				.name = "scalar compare-and-swap natural alignment",
				.instruction = atomic::compare_swap(size_code, true, true,
					2, 3, 4),
				.initial = initial,
				.expected = initial,
				.event = engine::ExpectedEvent::memory_fault,
				.native = atomic::lse(),
				.fault = {
					.address = atomic::data_address + 1,
					.access = arm64::MemoryAccess::write,
				},
				.compare_fault = true,
				.memory =
					std::span<const engine::MemoryRegion>{&unchanged, 1},
			});
		}
	}

	for (const bool compare_matches : {false, true}) {
		auto initial = atomic::state(23);
		initial.registers[2] = atomic::data_address;
		initial.registers[3] = compare_matches ? observed_value :
			(observed_value ^ 1U);
		initial.registers[4] = replacement_value;
		const auto read_only = atomic::region(page, page, true, false);
		harness.check({
			.name = "scalar compare-and-swap write permission",
			.instruction = atomic::compare_swap(3, true, true, 2, 3, 4),
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
	auto initial = atomic::state(24);
	initial.registers[2] = atomic::data_address;
	for (unsigned size_code = 0; size_code < 4; ++size_code)
		for (const bool acquire : {false, true})
			for (const bool release : {false, true})
				for (const unsigned rt2 : {0U, 1U, 15U, 30U})
					harness.check_illegal(
						"scalar compare-and-swap reserved Rt2",
						atomic::compare_swap(size_code, acquire, release,
							2, 3, 4, rt2),
						initial, atomic::lse());
	for (const bool wide : {false, true})
		for (const unsigned rt2 : {0U, 1U, 15U, 30U})
			harness.check_illegal(
				"unprivileged compare-and-swap allocation",
				atomic::unprivileged_compare_swap(wide, false, false,
					2, 3, 4, rt2),
				initial, atomic::lsui());
	for (const bool acquire : {false, true})
		for (const bool release : {false, true})
			harness.check_illegal(
				"unprivileged compare-and-swap reserved width",
				atomic::unprivileged_compare_swap(false, acquire, release,
					2, 3, 4),
				initial, atomic::lsui());
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	standard_variants(harness);
	unprivileged_variants(harness);
	faults(harness);
	allocation(harness);
	harness.print_summary("arm64-base-compare-swap");
	return 0;
}
