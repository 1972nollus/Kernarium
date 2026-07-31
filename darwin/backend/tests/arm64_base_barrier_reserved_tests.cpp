// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_barrier_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

namespace uml::backend::tests::base_barrier {
namespace {

template<std::size_t Count>
void reserved_options(EngineHarness &harness, std::string_view id,
	std::uint32_t base, const std::array<unsigned, Count> &options,
	std::uint64_t seed)
{
	for (const auto option : options)
		for (unsigned state_index = 0; state_index < 4U; ++state_index) {
			const auto instruction = encode_standard(base, option);
			const auto initial = seeded_state(
				seed ^
				(static_cast<std::uint64_t>(instruction) << 9U) ^
				state_index);
			char name[128];
			std::snprintf(name, sizeof(name),
				"%.*s reserved option=%u state=%u",
				static_cast<int>(id.size()), id.data(), option,
				state_index);
			/* The specification requires every reserved option here to
			 * execute as a full-system barrier, not trap. */
			check(harness, name, instruction, initial);
		}
}

void architectural_reserved_behavior(EngineHarness &harness)
{
	constexpr std::array dmb_reserved{0U, 4U, 8U, 12U};
	constexpr std::array dsb_reserved{8U, 12U};
	constexpr std::array isb_reserved{
		0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U,
		8U, 9U, 10U, 11U, 12U, 13U, 14U,
	};
	reserved_options(harness, "BASE-0150 DMB", dmb_base, dmb_reserved,
		0x4250'0000'0000'0000ULL);
	reserved_options(harness, "BASE-0152 DSB", dsb_base, dsb_reserved,
		0x4260'0000'0000'0000ULL);
	reserved_options(harness, "BASE-0181 ISB", isb_base, isb_reserved,
		0x4270'0000'0000'0000ULL);
}

} // namespace
} // namespace uml::backend::tests::base_barrier

int main()
{
	uml::backend::tests::EngineHarness harness;
	uml::backend::tests::base_barrier::architectural_reserved_behavior(
		harness);
	harness.print_summary("arm64-base-barrier-reserved");
	return 0;
}
