// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_ordered_memory_test_support.hpp"

#include <array>
#include <cstdio>

namespace ordered = arm64_ordered_memory_test;
namespace engine = uml::emulator::tests;

namespace {

void check_fault(engine::EngineHarness &harness,
	const ordered::PairVariant &variant, std::uint64_t target,
	std::uint64_t expected_fault, std::span<const engine::MemoryRegion> memory,
	std::string_view suffix, std::uint64_t seed)
{
	const auto total_size =
		2U * (std::size_t{1} << variant.size_code);
	auto initial = ordered::seeded_state(seed);
	ordered::set_base(initial, 2,
		variant.writeback && !variant.load ?
			target + total_size : target);
	char name[128];
	std::snprintf(name, sizeof(name), "%.*s %.*s",
		static_cast<int>(variant.name.size()), variant.name.data(),
		static_cast<int>(suffix.size()), suffix.data());
	harness.check({
		.name = name,
		.instruction = ordered::pair_instruction(variant, 2, 3, 4),
		.initial = initial,
		.expected = initial,
		.event = engine::ExpectedEvent::memory_fault,
		.native = ordered::native(variant),
		.fault = {
			.address = expected_fault,
			.access = variant.load ?
				ordered::arm64::MemoryAccess::read :
				ordered::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = memory,
	});
}

void alignment_permissions_and_restart(engine::EngineHarness &harness)
{
	std::uint64_t seed = 0x9100'0000'0000'0000ULL;
	for (const auto &variant : ordered::pair_variants) {
		check_fault(harness, variant, ordered::address + 1U,
			ordered::address + 1U, {}, "misaligned", seed++);

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
			ordered::address,
			std::span<const engine::MemoryRegion>{regions},
			variant.load ? "read-protected" : "write-protected",
			seed++);
	}
}

void second_page_fault_is_atomic(engine::EngineHarness &harness)
{
	std::uint64_t seed = 0x9200'0000'0000'0000ULL;
	const auto boundary = ordered::page_base + ordered::page_size;
	for (const auto &variant : ordered::pair_variants) {
		const auto element_size = std::size_t{1} << variant.size_code;
		const auto target = boundary - element_size;
		std::vector<std::byte> page(ordered::page_size);
		ordered::fill_page(page, static_cast<unsigned>(seed));
		const std::array regions{
			engine::MemoryRegion{
				.address = ordered::page_base,
				.initial = std::span<const std::byte>{page},
			},
		};
		check_fault(harness, variant, target, boundary,
			std::span<const engine::MemoryRegion>{regions},
			"cross-page second-page-unmapped", seed++);
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	alignment_permissions_and_restart(harness);
	second_page_fault_is_atomic(harness);
	harness.print_summary("arm64-base-ordered-pair-fault");
	return 0;
}
