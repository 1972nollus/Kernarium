// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <set>
#include <source_location>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x5f0'0000;

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

[[nodiscard]] constexpr std::uint32_t multiply_instruction(bool subtract,
	unsigned destination, unsigned left, unsigned right, unsigned addend)
{
	return (subtract ? 0x9b60'8000U : 0x9b60'0000U) |
		((right & 31U) << 16) | ((addend & 31U) << 10) |
		((left & 31U) << 5) | (destination & 31U);
}

void encoding_examples()
{
	require(multiply_instruction(false, 0, 1, 2, 3) == 0x9b62'0c20U);
	require(multiply_instruction(false, 30, 31, 31, 31) == 0x9b7f'7ffeU);
	require(multiply_instruction(true, 0, 1, 2, 3) == 0x9b62'8c20U);
	require(multiply_instruction(true, 30, 31, 31, 31) == 0x9b7f'fffeU);
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const bool subtract : {false, true})
		for (const bool left_zero : {false, true})
			for (const bool right_zero : {false, true})
				for (const bool addend_zero : {false, true})
					for (const bool destination_zero : {false, true}) {
						const auto built = gadgets::Builder{}.build(pc,
							std::array{multiply_instruction(subtract,
								destination_zero ? 31U : 4U,
								left_zero ? 31U : 1U,
								right_zero ? 31U : 2U,
								addend_zero ? 31U : 3U)});
							if (!(built.complete &&
							      built.program.instruction_count() == 1 &&
							      built.program.words().size() == 4)) {
								std::fprintf(stderr,
									"bad specialization sub=%u n=%u m=%u a=%u d=%u complete=%u instructions=%zu words=%zu\n",
									subtract, left_zero, right_zero,
									addend_zero, destination_zero,
									built.complete,
									built.program.instruction_count(),
									built.program.words().size());
								std::abort();
							}
						entries.insert(built.program.words().front());
					}
	require(entries.size() == 32);
}

} // namespace

int main()
{
	encoding_examples();
	production_specialization();
	return 0;
}
