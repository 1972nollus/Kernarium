// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_arithmetic_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace test = uml::emulator::tests::base_arithmetic;
namespace engine_tests = uml::emulator::tests;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace arm64 = uml::cpu::arm64;

namespace {

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

[[nodiscard]] constexpr std::uintptr_t register_offset(unsigned index)
{
	return index == 31U ? offsetof(arm64::CpuState, gadget_zero) :
		index * sizeof(std::uint64_t);
}

[[nodiscard]] constexpr std::uintptr_t packed_operand(
	unsigned destination, unsigned left, unsigned right)
{
	return register_offset(destination) |
		(register_offset(left) << 16U) |
		(register_offset(right) << 32U);
}

void require_regular_stream(const gadgets::BuildResult &built,
	std::uintptr_t expected_operand)
{
	if (!built.complete || built.program.words().size() != 4U) {
		std::fprintf(stderr,
			"arithmetic stream complete=%u words=%zu expected-operand=%#llx\n",
			built.complete ? 1U : 0U, built.program.words().size(),
			static_cast<unsigned long long>(expected_operand));
		fail("arithmetic instruction has unexpected gadget stream shape");
	}
	require(built.program.words()[1] == expected_operand,
		"arithmetic register offsets were not decoded at translation time");
}

void production_specialization()
{
	constexpr auto base = engine_tests::engine_test_code_base;
	constexpr std::uint32_t nop = 0xd503'201fU;
	const auto folded_nop = gadgets::Builder{}.build(base, std::array{nop});
	require(folded_nop.complete && folded_nop.program.words().size() == 3U,
		"NOP program did not build");

	std::set<std::uintptr_t> carry_entries;
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool flags : {false, true}) {
				const auto ordinary = gadgets::Builder{}.build(
					base, std::array{test::encode_carry(
						wide, subtract, flags, 7, 3,
						5)});
				require_regular_stream(ordinary,
					packed_operand(7, 3, 5));
				carry_entries.insert(
					ordinary.program.words().front());

				const auto same = gadgets::Builder{}.build(
					base, std::array{test::encode_carry(
						wide, subtract, flags, 7, 3,
						3)});
				require_regular_stream(same,
					packed_operand(7, 3, 3));
				require(same.program.words().front() !=
						ordinary.program.words().front(),
					"equal carry sources retained a duplicate runtime load");

				for (const auto sources :
				     {std::array{31U, 5U},
				      std::array{3U, 31U},
				      std::array{31U, 31U}}) {
					const auto zero = gadgets::Builder{}.build(
						base,
						std::array{test::encode_carry(
							wide, subtract,
							flags, 7,
							sources[0],
							sources[1])});
					require_regular_stream(zero,
						packed_operand(7,
							sources[0],
							sources[1]));
					require(zero.program.words().front() !=
							ordinary.program.words().front(),
						"zero carry source retained a redundant runtime load");
				}

				const auto discarded = gadgets::Builder{}.build(
					base, std::array{test::encode_carry(
						wide, subtract, flags, 31, 3,
						5)});
				if (!flags) {
					require(discarded.complete &&
							discarded.program.words().size() ==
								3U &&
							discarded.program.words().front() ==
								folded_nop.program.words().front(),
						"discarded flagless carry result was not folded to NOP");
				} else {
					require_regular_stream(discarded,
						packed_operand(31, 3, 5));
					require(discarded.program.words().front() !=
							ordinary.program.words().front(),
						"flag-only carry form retained a redundant destination store");
				}
			}
	require(carry_entries.size() == 8U,
		"carry width/operation forms did not select distinct gadgets");

	for (unsigned family = 0; family < 2U; ++family) {
		const unsigned operations = family == 0U ? 4U : 2U;
		for (const bool wide : {false, true})
			for (unsigned operation = 0; operation < operations;
			     ++operation) {
				const auto encode =
					[&](unsigned destination,
					    unsigned left,
					    unsigned right) {
						return family == 0U ?
							test::encode_variable_shift(
								wide,
								static_cast<
									test::ShiftOperation>(
									operation),
								destination,
								left, right) :
							test::encode_divide(
								wide,
								operation != 0U,
								destination,
								left, right);
					};
				const auto ordinary = gadgets::Builder{}.build(
					base, std::array{encode(7, 3, 5)});
				require_regular_stream(ordinary,
					packed_operand(7, 3, 5));
				const auto same = gadgets::Builder{}.build(
					base, std::array{encode(7, 3, 3)});
				require_regular_stream(same,
					packed_operand(7, 3, 3));
				require(same.program.words().front() !=
						ordinary.program.words().front(),
					"equal binary sources retained a duplicate runtime load");
				for (const auto sources :
				     {std::array{31U, 5U},
				      std::array{3U, 31U},
				      std::array{31U, 31U}}) {
					const auto zero =
						gadgets::Builder{}.build(
							base,
							std::array{encode(
								7,
								sources[0],
								sources[1])});
					require_regular_stream(zero,
						packed_operand(7,
							sources[0],
							sources[1]));
					require(zero.program.words().front() !=
							ordinary.program.words().front(),
						"zero binary source retained a redundant runtime load");
				}
				const auto discarded = gadgets::Builder{}.build(
					base, std::array{encode(31, 3, 5)});
				require(discarded.complete &&
						discarded.program.words().size() == 3U &&
						discarded.program.words().front() ==
							folded_nop.program.words().front(),
					"discarded flagless binary result was not folded to NOP");
			}
	}
}

} // namespace

int main()
{
	production_specialization();
	return 0;
}
