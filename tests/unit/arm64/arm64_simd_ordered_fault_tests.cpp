// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_ordered_test_support.hpp"

#include <array>
#include <cstdio>

namespace ordered = arm64_simd_ordered_test;
namespace engine = uml::emulator::tests;

namespace {

template <typename Variants>
void check_faults(engine::EngineHarness &harness, const Variants &variants,
	std::uint64_t &seed)
{
	for (const auto &variant : variants) {
		if (variant.size > 1U) {
			auto initial = ordered::seeded_state(seed++);
			ordered::set_base(initial, 2, ordered::address + 1U);
			char name[112];
			std::snprintf(name, sizeof(name), "%.*s misaligned",
				static_cast<int>(variant.name.size()),
				variant.name.data());
			harness.check({
				.name = name,
				.instruction =
					ordered::simd_instruction(variant, 0, 2, 3),
				.initial = initial,
				.expected = initial,
				.event = engine::ExpectedEvent::memory_fault,
				.native = ordered::native(variant),
				.fault = {
					.address = ordered::address + 1U,
					.access = variant.load ?
						ordered::arm64::MemoryAccess::read :
						ordered::arm64::MemoryAccess::write,
				},
				.compare_fault = true,
			});
		}

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
		auto initial = ordered::seeded_state(seed++);
		ordered::set_base(initial, 2, ordered::address);
		char name[112];
		std::snprintf(name, sizeof(name), "%.*s %s-protected",
			static_cast<int>(variant.name.size()), variant.name.data(),
			variant.load ? "read" : "write");
		harness.check({
			.name = name,
			.instruction =
				ordered::simd_instruction(variant, 0, 2, 3),
			.initial = initial,
			.expected = initial,
			.event = engine::ExpectedEvent::memory_fault,
			.native = ordered::native(variant),
			.fault = {
				.address = ordered::address,
				.access = variant.load ?
					ordered::arm64::MemoryAccess::read :
					ordered::arm64::MemoryAccess::write,
			},
			.compare_fault = true,
			.memory = std::span<const engine::MemoryRegion>{regions},
		});
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	std::uint64_t seed = 0xc100'0000'0000'0000ULL;
	check_faults(harness, ordered::simd_unscaled_variants, seed);
	check_faults(harness, ordered::simd_lane_variants, seed);
	harness.print_summary("arm64-simd-ordered-fault");
	return 0;
}
