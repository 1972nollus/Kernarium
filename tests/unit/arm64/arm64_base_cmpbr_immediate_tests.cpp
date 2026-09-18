// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_cmpbr_test_support.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <set>

namespace test = cmpbr_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

constexpr std::array valid_conditions{0U, 1U, 2U, 3U, 6U, 7U};

[[nodiscard]] constexpr std::int64_t signed_operand(
	bool wide, std::uint64_t value)
{
	return wide ? std::bit_cast<std::int64_t>(value) :
		std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

[[nodiscard]] constexpr std::uint32_t instruction(bool wide, unsigned cc,
	unsigned immediate, unsigned imm9, unsigned rt)
{
	return 0x7500'0000U | (wide ? 0x8000'0000U : 0) |
		((cc & 7U) << 21) | ((immediate & 63U) << 15) |
		((imm9 & 0x1ffU) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr bool takes(bool wide, unsigned cc,
	std::uint64_t value, unsigned immediate)
{
	const auto masked = wide ? value : value & 0xffff'ffffU;
	const auto signed_value = signed_operand(wide, masked);
	switch (cc) {
	case 0: return signed_value > static_cast<std::int64_t>(immediate);
	case 1: return signed_value < static_cast<std::int64_t>(immediate);
	case 2: return masked > immediate;
	case 3: return masked < immediate;
	case 6: return masked == immediate;
	case 7: return masked != immediate;
	default: return false;
	}
}

void encoding_examples()
{
	test::require(instruction(false, 0, 7, 0, 1) == 0x7503'8001U);
	test::require(instruction(true, 1, 63, 0, 2) == 0xf53f'8002U);
	test::require(instruction(false, 6, 0, 0x100, 31) == 0x75c0'201fU);
}

void aliases()
{
	/* CBGE/CBHS encode x >= N as x > N-1. CBLE/CBLS encode x <= N
	 * as x < N+1. Check the full assembler-visible immediate ranges.
	 */
	for (const bool wide : {false, true})
		for (const std::uint64_t value : {std::uint64_t{0},
			std::uint64_t{1}, std::uint64_t{63},
			std::uint64_t{64}, ~std::uint64_t{0}}) {
			for (unsigned alias = 1; alias <= 64; ++alias) {
				test::require(takes(wide, 0, value, alias - 1) ==
					(signed_operand(wide, value) >=
					 static_cast<std::int64_t>(alias)));
				test::require(takes(wide, 2, value, alias - 1) ==
					((wide ? value : value & 0xffff'ffffU) >= alias));
			}
			for (int alias = -1; alias <= 62; ++alias) {
				const auto encoded = static_cast<unsigned>(alias + 1);
				const auto signed_value = signed_operand(wide, value);
				test::require(takes(wide, 1, value, encoded) ==
					(signed_value <= alias));
				test::require(takes(wide, 3, value, encoded) ==
					(alias >= 0 &&
					 (wide ? value : value & 0xffff'ffffU) <=
					 static_cast<std::uint64_t>(alias)));
			}
		}
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	std::set<std::uintptr_t> constants;
	for (const bool wide : {false, true})
		for (const auto cc : valid_conditions) {
			const auto built = gadgets::Builder{}.build(test::base,
				std::array{instruction(wide, cc, 17, 5, 1)});
			test::require(built.complete &&
				built.program.words().size() == 4);
			entries.insert(built.program.words().front());

			const auto folded = gadgets::Builder{}.build(test::base,
				std::array{instruction(wide, cc, 17, 5, 31)});
			test::require(folded.complete &&
				folded.program.words().size() == 2);
			constants.insert(folded.program.words().front());
		}
	test::require(entries.size() == 12 && constants.size() == 1);
}

} // namespace

int main()
{
	encoding_examples();
	aliases();
	production_specialization();
	return 0;
}
