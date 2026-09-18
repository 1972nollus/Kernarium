// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_multiply_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <set>

namespace uml::emulator::tests::base_multiply {
namespace {

namespace gadgets = arm64::gadgets;

[[noreturn]] void fail(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	std::abort();
}

void require(bool condition, const char *message)
{
	if (!condition)
		fail(message);
}

void production_specialization()
{
	constexpr auto base = engine_test_code_base;
	constexpr std::uint32_t nop = 0xd503'201fU;
	const auto folded_nop = gadgets::Builder{}.build(base, std::array{nop});
	require(folded_nop.complete, "NOP program did not build");

	const auto entry_for = [](const FormCase &form, unsigned left,
				       unsigned right, unsigned addend) {
		const auto instruction = encode(form, left, right, addend, 7U);
		const auto program =
			gadgets::Builder{}.build(base, std::array{instruction});
		require(program.complete && program.program.words().size() == 4U,
			"multiply selector probe has unexpected stream shape");
		return program.program.words().front();
	};

	std::set<std::uintptr_t> entries;
	for (const auto &form : legal_forms)
		for (const bool left_zero : {false, true})
			for (const bool right_zero : {false, true})
				for (const bool addend_zero : {false, true}) {
					if (is_high(form) && !addend_zero)
						continue;
					const auto instruction = encode(
						form, left_zero ? 31U : 3U,
						right_zero ? 31U : 5U,
						addend_zero ? 31U : 11U, 7U);
					const auto program = gadgets::Builder{}.build(
						base, std::array{instruction});
					require(program.complete &&
							program.program.words().size() == 4U,
						"multiply form has unexpected specialized "
						"stream shape");
					const auto operand = program.program.words()[1];
					require((operand & 0xffffU) ==
							7U * sizeof(std::uint64_t),
						"multiply destination was not resolved at "
						"translation time");
					entries.insert(program.program.words().front());

					const auto discard_instruction =
						(instruction & ~31U) | 31U;
					const auto discarded = gadgets::Builder{}.build(
						base, std::array{discard_instruction});
					require(discarded.complete &&
							discarded.program.words().size() == 3U,
						"discarded multiply retained runtime operands");
					require(discarded.program.words().front() ==
							folded_nop.program.words().front(),
						"discarded multiply was not folded to NOP");
				}
	require(entries.size() == 36U,
		"multiply source/addend forms did not select exactly 36 live gadgets");

	for (const auto &form : legal_forms)
		for (const bool addend_zero : {false, true}) {
			if (is_high(form) && !addend_zero)
				continue;
			const unsigned addend = addend_zero ? 31U : 11U;
			const auto ordinary = entry_for(form, 3U, 5U, addend);
			const auto left_zero = entry_for(form, 31U, 5U, addend);
			const auto right_zero = entry_for(form, 3U, 31U, addend);
			const auto both_zero = entry_for(form, 31U, 31U, addend);
			require(ordinary != left_zero,
				"ordinary multiply selected the zero-product gadget");
			require(left_zero == right_zero && left_zero == both_zero,
				"zero-product multiply did not share one gadget");
		}
}

} // namespace
} // namespace uml::emulator::tests::base_multiply

int main()
{
	uml::emulator::tests::base_multiply::production_specialization();
	return 0;
}
