// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_lsui_pair_test_support.hpp"

#include <cstdio>
#include <span>
#include <vector>

namespace lsui = arm64_base_lsui_pair_test;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::backend::tests;

namespace {

[[nodiscard]] std::uint64_t base_for_target(
	std::uint64_t target, lsui::Mode mode, unsigned imm7)
{
	return mode == lsui::Mode::post_index ?
		target : target - static_cast<std::uint64_t>(
			lsui::displacement(imm7));
}

void stack_alignment_faults(
	engine::EngineHarness &harness, lsui::ExpectedCounts &counts)
{
	for (unsigned index = 0; index < lsui::variants.size(); ++index) {
		const auto &variant = lsui::variants[index];
		auto memory = std::vector<std::byte>(lsui::page_size);
		lsui::fill(memory, 0x900U + index);
		auto initial = atomic::state(0x9000U + index);
		initial.sp = lsui::base_address + 8U;
		initial.registers[3] = 0x1234'5678'9abc'def0ULL;
		initial.registers[4] = 0xfedc'ba98'7654'3210ULL;
		const auto region = engine::MemoryRegion{
			.address = lsui::data_page,
			.initial = std::span<const std::byte>{memory},
		};
		const auto native = atomic::lsui();
		char name[128];
		std::snprintf(name, sizeof(name), "%.*s misaligned SP",
			static_cast<int>(variant.mnemonic.size()),
			variant.mnemonic.data());
		harness.check({
			.name = name,
			.instruction = lsui::instruction(
				variant.load, variant.mode, 3, 31, 3, 4),
			.initial = initial,
			.expected = initial,
			.event = engine::ExpectedEvent::memory_fault,
			.native = native,
			.fault = {
				.address = initial.sp,
				.access = variant.load ?
					lsui::arm64::MemoryAccess::read :
					lsui::arm64::MemoryAccess::write,
			},
			.compare_fault = true,
			.memory = std::span<const engine::MemoryRegion>{&region, 1},
		});
		counts.add(native);
	}
}

void cross_page_success(
	engine::EngineHarness &harness, lsui::ExpectedCounts &counts)
{
	const auto boundary = lsui::data_page + lsui::page_size;
	for (unsigned index = 0; index < lsui::variants.size(); ++index) {
		const auto &variant = lsui::variants[index];
		for (const unsigned split : {4U, 8U}) {
			const auto target = boundary - split;
			const unsigned imm7 = index % 2U == 0U ? 3U : 0x7dU;
			auto memory = std::vector<std::byte>(2U * lsui::page_size);
			lsui::fill(memory, 0xa00U + index * 2U + split);
			auto initial = atomic::state(0xa000U + index * 2U + split);
			initial.registers[20] =
				base_for_target(target, variant.mode, imm7);
			initial.registers[3] = 0xf123'4567'89ab'cdefULL;
			initial.registers[4] = 0x8123'4567'0bad'f00dULL;
			char name[128];
			std::snprintf(name, sizeof(name),
				"%.*s cross-page success split=%u",
				static_cast<int>(variant.mnemonic.size()),
				variant.mnemonic.data(), split);
			lsui::check(harness, counts, name, variant, imm7, 20, 3, 4,
				initial, lsui::data_page, memory);
		}
	}
}

void cross_page_fault_atomicity(
	engine::EngineHarness &harness, lsui::ExpectedCounts &counts)
{
	const auto boundary = lsui::data_page + lsui::page_size;
	for (unsigned index = 0; index < lsui::variants.size(); ++index) {
		const auto &variant = lsui::variants[index];
		for (const unsigned split : {4U, 8U}) {
			const auto target = boundary - split;
			const unsigned imm7 = index % 2U == 0U ? 5U : 0x7bU;
			auto memory = std::vector<std::byte>(lsui::page_size);
			lsui::fill(memory, 0xb00U + index * 2U + split);
			auto initial = atomic::state(0xb000U + index * 2U + split);
			initial.registers[20] =
				base_for_target(target, variant.mode, imm7);
			initial.registers[3] = 0xf123'4567'89ab'cdefULL;
			initial.registers[4] = 0x8123'4567'0bad'f00dULL;
			const auto region = engine::MemoryRegion{
				.address = lsui::data_page,
				.initial = std::span<const std::byte>{memory},
			};
			const auto native = atomic::lsui();
			char name[128];
			std::snprintf(name, sizeof(name),
				"%.*s cross-page fault split=%u",
				static_cast<int>(variant.mnemonic.size()),
				variant.mnemonic.data(), split);
			harness.check({
				.name = name,
				.instruction = lsui::instruction(
					variant.load, variant.mode, imm7, 20, 3, 4),
				.initial = initial,
				.expected = initial,
				.event = engine::ExpectedEvent::memory_fault,
				.native = native,
				.fault = {
					.address = boundary,
					.access = variant.load ?
						lsui::arm64::MemoryAccess::read :
						lsui::arm64::MemoryAccess::write,
				},
				.compare_fault = true,
				.memory = std::span<const engine::MemoryRegion>{&region, 1},
			});
			counts.add(native);
		}
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	lsui::ExpectedCounts counts;
	stack_alignment_faults(harness, counts);
	cross_page_success(harness, counts);
	cross_page_fault_atomicity(harness, counts);
	lsui::require_counts(harness, counts);
	harness.print_summary("arm64-base-lsui-pair-fault");
}
