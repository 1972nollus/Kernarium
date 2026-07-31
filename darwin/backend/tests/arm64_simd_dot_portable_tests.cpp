// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_dot_test_support.hpp"

namespace test = arm64_simd_dot_test;

namespace {

[[nodiscard]] test::arm64::CpuState edge_state(unsigned seed,
	test::Registers registers, unsigned pattern)
{
	auto initial = test::state(seed);
	if (pattern == 0U) {
		initial.vectors[registers.destination].bytes.fill(std::byte{0xff});
		initial.vectors[registers.left].bytes.fill(std::byte{0xff});
		initial.vectors[registers.right].bytes.fill(std::byte{0xff});
	} else if (pattern == 1U) {
		initial.vectors[registers.destination].bytes.fill(std::byte{0x7f});
		initial.vectors[registers.left].bytes.fill(std::byte{0x80});
		initial.vectors[registers.right].bytes.fill(std::byte{0xff});
	}
	return initial;
}

void every_portable_body_matches_the_spec_oracle()
{
	test::engine_tests::EngineHarness harness{{
		.force_software_dotprod = true,
		.force_software_i8mm = true,
	}};
	constexpr test::Registers registers{7U, 7U, 23U};
	for (const auto mode : test::vector_modes)
		for (const bool q : {false, true})
			for (unsigned pattern = 0; pattern < 3U; ++pattern)
				test::check(harness, "dot portable vector",
					test::Form::vector, mode, q, registers, 0U,
					edge_state(31U + pattern + q +
						static_cast<unsigned>(mode),
						registers, pattern));
	for (const auto mode : test::indexed_modes)
		for (const bool q : {false, true})
			for (unsigned index = 0; index < 4U; ++index)
				for (unsigned pattern = 0; pattern < 3U; ++pattern)
					test::check(harness, "dot portable indexed",
						test::Form::indexed, mode, q, registers, index,
						edge_state(73U + pattern + index + q +
							static_cast<unsigned>(mode),
							registers, pattern));
	harness.print_summary("simd-dot-portable");
}

} // namespace

int main()
{
	every_portable_body_matches_the_spec_oracle();
	return 0;
}
