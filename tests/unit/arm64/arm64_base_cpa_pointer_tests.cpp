// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <source_location>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x5e0'0000;

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

[[nodiscard]] constexpr std::uint32_t pointer_instruction(bool subtract,
	unsigned destination, unsigned base, unsigned offset, unsigned shift)
{
	return (subtract ? 0xda00'2000U : 0x9a00'2000U) |
		((offset & 31U) << 16) | ((shift & 7U) << 10) |
		((base & 31U) << 5) | (destination & 31U);
}

void encoding_examples()
{
	require(pointer_instruction(false, 0, 1, 2, 0) == 0x9a02'2020U);
	require(pointer_instruction(false, 31, 31, 31, 7) == 0x9a1f'3fffU);
	require(pointer_instruction(true, 30, 1, 2, 3) == 0xda02'2c3eU);
	require(pointer_instruction(true, 31, 31, 31, 7) == 0xda1f'3fffU);
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const bool subtract : {false, true})
		for (unsigned shift = 0; shift < 8; ++shift)
			for (const bool base_sp : {false, true})
				for (const bool offset_zero : {false, true})
					for (const bool destination_sp : {false, true}) {
						const auto built = gadgets::Builder{}.build(pc,
							std::array{pointer_instruction(subtract,
								destination_sp ? 31U : 3U,
								base_sp ? 31U : 1U,
								offset_zero ? 31U : 2U, shift)});
						require(built.complete &&
							built.program.instruction_count() == 1 &&
							built.program.words().size() == 4);
						entries.insert(built.program.words().front());
					}
	require(entries.size() == 128);
}

} // namespace

int main()
{
	encoding_examples();
	production_specialization();
	return 0;
}
