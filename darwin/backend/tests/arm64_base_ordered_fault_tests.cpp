// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_ordered_memory_test_support.hpp"

#include <array>
#include <cstdio>

namespace ordered = arm64_ordered_memory_test;
namespace engine = uml::backend::tests;

namespace {

void check_fault(engine::EngineHarness &harness,
	const ordered::ScalarVariant &variant, std::uint64_t target,
	std::span<const engine::MemoryRegion> memory, std::string_view suffix,
	std::uint64_t seed)
{
	constexpr int offset = 0;
	constexpr unsigned rn = 2;
	constexpr unsigned rt = 3;
	auto initial = ordered::seeded_state(seed);
	ordered::set_base(initial, rn,
		ordered::scalar_base_for_target(variant, target, offset));
	char name[120];
	ordered::describe(name, sizeof(name), variant.name, suffix, rn, rt);
	harness.check({
		.name = name,
		.instruction =
			ordered::scalar_instruction(variant, offset, rn, rt),
		.initial = initial,
		.expected = initial,
		.event = engine::ExpectedEvent::memory_fault,
		.native = ordered::native(variant),
		.fault = {
			.address = target,
			.access = variant.load ?
				ordered::arm64::MemoryAccess::read :
				ordered::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = memory,
	});
}

void alignment_and_permissions(engine::EngineHarness &harness)
{
	std::uint64_t seed = 0x6100'0000'0000'0000ULL;
	for (const auto &variant : ordered::scalar_variants) {
		const auto size = std::size_t{1} << variant.size_code;
		if (size > 1U)
			check_fault(harness, variant, ordered::address + 1U, {},
				"misaligned", seed++);

		std::vector<std::byte> page(ordered::page_size);
		ordered::fill_page(page, static_cast<unsigned>(seed));
		const std::array regions{
			engine::MemoryRegion{
				.address = ordered::page_base,
				.initial = std::span<const std::byte>{page},
				.readable = !variant.load,
				.writable = variant.load,
			},
		};
		check_fault(harness, variant, ordered::address,
			std::span<const engine::MemoryRegion>{regions},
			variant.load ? "read-protected" : "write-protected",
			seed++);
	}
}

void writeback_restart_state(engine::EngineHarness &harness)
{
	std::uint64_t seed = 0x6200'0000'0000'0000ULL;
	for (const auto &variant : ordered::scalar_variants) {
		if (variant.instruction_class != ordered::ScalarClass::writeback)
			continue;
		for (const unsigned rt : {2U, 3U, 31U}) {
			constexpr unsigned rn = 2;
			const auto size = std::size_t{1} << variant.size_code;
			const auto target = variant.load ?
				ordered::unmapped_address :
				ordered::unmapped_address - size;
			auto initial = ordered::seeded_state(seed++);
			ordered::set_base(initial, rn, ordered::unmapped_address);
			char name[120];
			ordered::describe(name, sizeof(name), variant.name,
				"restart-unmapped", rn, rt);
			harness.check({
				.name = name,
				.instruction = ordered::scalar_instruction(
					variant, 0, rn, rt),
				.initial = initial,
				.expected = initial,
				.event = engine::ExpectedEvent::memory_fault,
				.native = ordered::native(variant),
				.fault = {
					.address = target,
					.access = variant.load ?
						ordered::arm64::MemoryAccess::read :
						ordered::arm64::MemoryAccess::write,
				},
				.compare_fault = true,
			});
		}
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	alignment_and_permissions(harness);
	writeback_restart_state(harness);
	harness.print_summary("arm64-base-ordered-fault");
	return 0;
}
