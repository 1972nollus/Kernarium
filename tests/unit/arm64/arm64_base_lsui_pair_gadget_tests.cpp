// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_lsui_pair_test_support.hpp"

#include <array>
#include <span>
#include <vector>

namespace lsui = arm64_base_lsui_pair_test;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

namespace {

void translation_specialized_mode_blocks(
	engine::EngineHarness &harness, lsui::ExpectedCounts &counts)
{
	for (unsigned mode = 0; mode < 4; ++mode) {
		const auto store = lsui::variants[mode * 2U];
		const auto load = lsui::variants[mode * 2U + 1U];
		const unsigned imm7 = mode == 0U ? 5U :
			mode == 1U ? 3U : mode == 2U ? 0x7cU : 0x7eU;
		const auto target = lsui::base_address;
		const auto base = mode == lsui::code(lsui::Mode::post_index) ?
			target : target - static_cast<std::uint64_t>(
				lsui::displacement(imm7));
		auto initial = atomic::state(0xc000U + mode);
		initial.registers[10] = base;
		initial.registers[11] = base;
		initial.registers[2] = 0xf123'4567'89ab'cdefULL;
		initial.registers[3] = 0x8123'4567'0bad'f00dULL;
		auto expected = initial;
		expected.registers[4] = initial.registers[2];
		expected.registers[5] = initial.registers[3];
		if (lsui::writes_back(store.mode))
			expected.registers[10] = lsui::adjusted(base, imm7);
		if (lsui::writes_back(load.mode))
			expected.registers[11] = lsui::adjusted(base, imm7);
		auto initial_memory = std::vector<std::byte>(lsui::page_size);
		lsui::fill(initial_memory, 0xc00U + mode);
		auto expected_memory = initial_memory;
		const auto offset = static_cast<std::size_t>(
			target - lsui::data_page);
		lsui::put(expected_memory, offset, initial.registers[2]);
		lsui::put(expected_memory, offset + 8U, initial.registers[3]);
		const std::array code{
			engine::ProgramWord{
				.offset = 0,
				.instruction = lsui::instruction(
					false, store.mode, imm7, 10, 2, 3),
			},
			engine::ProgramWord{
				.offset = sizeof(std::uint32_t),
				.instruction = lsui::instruction(
					true, load.mode, imm7, 11, 4, 5),
			},
			engine::ProgramWord{
				.offset = 2U * sizeof(std::uint32_t),
				.instruction = atomic::terminal_system_call,
			},
		};
		expected.pc = engine::engine_test_code_base +
			3U * sizeof(std::uint32_t);
		const engine::MemoryRegion memory{
			.address = lsui::data_page,
			.initial = std::span<const std::byte>{initial_memory},
			.expected = std::span<const std::byte>{expected_memory},
		};
		const auto native = atomic::lsui();
		harness.check_program({
			.name = "translation-specialized LSUI store/load mode block",
			.code = std::span<const engine::ProgramWord>{code},
			.initial = initial,
			.expected = expected,
			.native = native,
			.memory = std::span<const engine::MemoryRegion>{&memory, 1},
		});
		counts.add(native);
	}
}

void stores_invalidate_reservations(
	engine::EngineHarness &harness, lsui::ExpectedCounts &counts)
{
	for (unsigned mode = 0; mode < 4; ++mode) {
		const auto store = lsui::variants[mode * 2U];
		atomic::Page page{};
		atomic::put(page, atomic::data_offset,
			0xf123'4567'89ab'cdefULL, 8);
		atomic::put(page, atomic::data_offset + 8U,
			0x8123'4567'0bad'f00dULL, 8);
		const auto memory = atomic::region(page, page);
		const std::array code{
			engine::ProgramWord{
				0, atomic::scalar_exclusive(3, true, false, 2, 5)},
			engine::ProgramWord{
				4, lsui::instruction(false, store.mode, 0, 2, 3, 4)},
			engine::ProgramWord{
				8, atomic::scalar_exclusive(3, false, false, 2, 7, 6)},
			engine::ProgramWord{12, atomic::clrex(3)},
			engine::ProgramWord{16, atomic::terminal_system_call},
		};
		auto initial = atomic::state(0xd000U + mode);
		initial.registers[2] = atomic::data_address;
		initial.registers[3] = 0xf123'4567'89ab'cdefULL;
		initial.registers[4] = 0x8123'4567'0bad'f00dULL;
		initial.registers[7] = ~initial.registers[3];
		auto expected = initial;
		expected.registers[5] = initial.registers[3];
		expected.registers[6] = 1;
		atomic::clear_monitor(expected);
		expected.pc = engine::engine_test_code_base +
			5U * sizeof(std::uint32_t);
		const auto native = atomic::lsui();
		harness.check_program({
			.name = "LSUI pair store invalidates reservation",
			.code = std::span<const engine::ProgramWord>{code},
			.initial = initial,
			.expected = expected,
			.native = native,
			.memory = std::span<const engine::MemoryRegion>{&memory, 1},
		});
		counts.add(native);
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	lsui::ExpectedCounts counts;
	translation_specialized_mode_blocks(harness, counts);
	stores_invalidate_reservations(harness, counts);
	lsui::require_counts(harness, counts);
	harness.print_summary("arm64-base-lsui-pair-gadget");
}
