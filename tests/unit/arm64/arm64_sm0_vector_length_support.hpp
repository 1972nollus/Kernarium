// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/*
 * The SM = 0 vector-length family (cpu/arm64/detail/sm0_vector_length.hpp)
 * as the tests state it: the words of every form and a model of what each
 * does to the general registers at a vector length of 16 bytes, written
 * from DecodePredCount() and the instructions' pseudocode and independent
 * of the library under test.
 */
#include <array>
#include <bit>
#include <cstdint>
#include <utility>
#include <vector>

namespace uml::emulator::tests::sm0_vector_length {

inline constexpr unsigned vector_bytes = 16;

/* CNT[BHWD] Xd, pattern, MUL #m (size 0-3 for B, H, W, D). */
[[nodiscard]] constexpr std::uint32_t cnt(unsigned size, unsigned pattern,
	unsigned multiplier, unsigned rd) noexcept
{
	return 0x0420'e000U | (size << 22) | ((multiplier - 1U) << 16) |
		(pattern << 5) | rd;
}

/* INC[BHWD] and DEC[BHWD] Xdn, pattern, MUL #m. */
[[nodiscard]] constexpr std::uint32_t inc_dec(bool decrement, unsigned size,
	unsigned pattern, unsigned multiplier, unsigned rdn) noexcept
{
	return 0x0430'e000U | (decrement ? 0x400U : 0U) | (size << 22) |
		((multiplier - 1U) << 16) | (pattern << 5) | rdn;
}

[[nodiscard]] constexpr std::uint32_t rdvl(int immediate, unsigned rd) noexcept
{
	return 0x04bf'5000U | ((static_cast<unsigned>(immediate) & 63U) << 5) |
		rd;
}

/* ADDVL (predicate false) and ADDPL (true) Xd|SP, Xn|SP, #imm. */
[[nodiscard]] constexpr std::uint32_t add_length(bool predicate,
	unsigned rd, unsigned rn, int immediate) noexcept
{
	return (predicate ? 0x0460'5000U : 0x0420'5000U) | (rn << 16) |
		((static_cast<unsigned>(immediate) & 63U) << 5) | rd;
}

/* DecodePredCount(pattern, elements): the element count a predicate
 * constraint names; the reserved patterns name none. */
[[nodiscard]] constexpr unsigned pattern_count(unsigned pattern,
	unsigned elements) noexcept
{
	unsigned count = 0;
	if (pattern == 0U)
		count = std::bit_floor(elements);
	else if (pattern <= 8U)
		count = pattern;
	else if (pattern >= 9U && pattern <= 13U)
		count = 16U << (pattern - 9U);
	else if (pattern == 29U)
		count = elements - elements % 4U;
	else if (pattern == 30U)
		count = elements - elements % 3U;
	else if (pattern == 31U)
		count = elements;
	return count <= elements ? count : 0U;
}

/* The general registers a family word reads and writes. */
struct Registers final {
	std::array<std::uint64_t, 31> x{};
	std::uint64_t sp{};
};

/* What the word does to `registers`; false for a word outside the family. */
[[nodiscard]] constexpr bool model(std::uint32_t word,
	Registers &registers) noexcept
{
	const unsigned rd = word & 31U;
	const unsigned rn = (word >> 16) & 31U;
	const unsigned size = (word >> 22) & 3U;
	const unsigned elements = vector_bytes >> size;
	const unsigned multiplier = ((word >> 16) & 15U) + 1U;
	const unsigned pattern = (word >> 5) & 31U;
	const std::int64_t immediate = ((word >> 5) & 63U) >= 32U ?
		static_cast<std::int64_t>((word >> 5) & 63U) - 64 :
		static_cast<std::int64_t>((word >> 5) & 63U);
	const auto write = [&](std::uint64_t value) {
		if (rd != 31U)
			registers.x[rd] = value;
	};
	if ((word & 0xff30'fc00U) == 0x0420'e000U) { // CNT
		write(static_cast<std::uint64_t>(pattern_count(pattern, elements)) *
			multiplier);
		return true;
	}
	if ((word & 0xff30'fc00U) == 0x0430'e000U ||
	    (word & 0xff30'fc00U) == 0x0430'e400U) { // INC, DEC
		const std::uint64_t count =
			static_cast<std::uint64_t>(pattern_count(pattern, elements)) *
			multiplier;
		if (rd != 31U)
			registers.x[rd] += (word & 0x400U) != 0U ? 0U - count : count;
		return true;
	}
	if ((word & 0xffff'f800U) == 0x04bf'5000U) { // RDVL
		write(static_cast<std::uint64_t>(immediate *
			static_cast<std::int64_t>(vector_bytes)));
		return true;
	}
	if ((word & 0xffa0'f800U) == 0x0420'5000U) { // ADDVL, ADDPL
		const std::int64_t scale = (word & 0x0040'0000U) != 0U ?
			vector_bytes / 8 : vector_bytes;
		const std::uint64_t base = rn == 31U ? registers.sp :
			registers.x[rn];
		const std::uint64_t result = base +
			static_cast<std::uint64_t>(immediate * scale);
		if (rd == 31U)
			registers.sp = result;
		else
			registers.x[rd] = result;
		return true;
	}
	return false;
}

/* Family words to run: every size, every predicate pattern and several
 * multipliers of CNT, INC and DEC, each in destination registers that cover
 * XZR and the highest register; RDVL for every immediate; ADDVL and ADDPL
 * for every immediate, over distinct, equal and SP registers. */
[[nodiscard]] inline std::vector<std::uint32_t> words()
{
	std::vector<std::uint32_t> result;
	for (unsigned size = 0; size < 4U; ++size)
		for (unsigned pattern = 0; pattern < 32U; ++pattern)
			for (const unsigned multiplier : {1U, 2U, 7U, 16U})
				for (const unsigned rd : {0U, 5U, 30U, 31U}) {
					result.push_back(cnt(size, pattern, multiplier, rd));
					result.push_back(
						inc_dec(false, size, pattern, multiplier, rd));
					result.push_back(
						inc_dec(true, size, pattern, multiplier, rd));
				}
	for (int immediate = -32; immediate < 32; ++immediate) {
		for (const unsigned rd : {0U, 5U, 30U, 31U})
			result.push_back(rdvl(immediate, rd));
		for (const bool predicate : {false, true})
			for (const auto [rd, rn] :
			     {std::pair{0U, 1U}, std::pair{3U, 3U}, std::pair{31U, 31U},
			      std::pair{2U, 31U}, std::pair{31U, 4U},
			      std::pair{30U, 30U}})
				result.push_back(add_length(predicate, rd, rn, immediate));
	}
	return result;
}

} // namespace uml::emulator::tests::sm0_vector_length
