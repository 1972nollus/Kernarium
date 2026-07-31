// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_cmpbr_test_support.hpp"

#include <array>
#include <cstdint>
#include <set>

namespace test = cmpbr_test;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::array valid_conditions{0U, 1U, 2U, 3U, 6U, 7U};
constexpr std::array widths{8U, 16U, 32U, 64U};

[[nodiscard]] constexpr std::uint32_t instruction(unsigned width, unsigned cc,
	unsigned rm, unsigned imm9, unsigned rt)
{
	const auto size = width == 8 ? 0x0000'8000U :
		width == 16 ? 0x0000'c000U :
		width == 64 ? 0x8000'0000U : 0U;
	return 0x7400'0000U | size | ((cc & 7U) << 21) |
		((rm & 31U) << 16) | ((imm9 & 0x1ffU) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint64_t width_mask(unsigned width)
{
	return width == 64 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << width) - 1;
}

[[nodiscard]] constexpr std::int64_t signed_value(
	std::uint64_t value, unsigned width)
{
	const auto masked = value & width_mask(width);
	const auto sign = std::uint64_t{1} << (width - 1);
	return (masked & sign) != 0 ?
		static_cast<std::int64_t>(masked | ~width_mask(width)) :
		static_cast<std::int64_t>(masked);
}

[[nodiscard]] constexpr bool takes(unsigned width, unsigned cc,
	std::uint64_t left_value, std::uint64_t right_value)
{
	const auto left = left_value & width_mask(width);
	const auto right = right_value & width_mask(width);
	const auto signed_left = signed_value(left, width);
	const auto signed_right = signed_value(right, width);
	switch (cc) {
	case 0: return signed_left > signed_right;
	case 1: return signed_left >= signed_right;
	case 2: return left > right;
	case 3: return left >= right;
	case 6: return left == right;
	case 7: return left != right;
	default: return false;
	}
}

void encoding_examples()
{
	test::require(instruction(32, 0, 2, 0, 1) == 0x7402'0001U);
	test::require(instruction(64, 1, 3, 0, 2) == 0xf423'0002U);
	test::require(instruction(8, 6, 2, 0, 1) == 0x74c2'8001U);
	test::require(instruction(8, 7, 31, 0, 31) == 0x74ff'801fU);
	test::require(instruction(16, 6, 2, 0, 1) == 0x74c2'c001U);
	test::require(instruction(16, 7, 31, 0, 31) == 0x74ff'c01fU);
}

void exhaustive_fields(test::Harness &harness)
{
	constexpr std::array offsets{0U, 1U, 0xffU, 0x100U, 0x1ffU};
	const auto initial = test::state();
	for (const auto width : widths)
		for (const auto cc : valid_conditions)
			for (unsigned rt = 0; rt < 32; ++rt)
				for (unsigned rm = 0; rm < 32; ++rm)
					for (const auto imm9 : offsets) {
						const auto left = rt == 31 ? 0 :
							initial.registers[rt];
						const auto right = rm == 31 ? 0 :
							initial.registers[rm];
						harness.check(instruction(width, cc, rm,
							imm9, rt), initial,
							takes(width, cc, left, right),
							test::displacement(imm9));
					}
}

void signed_unsigned_boundaries(test::Harness &harness)
{
	constexpr std::array values{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0x7f},
		std::uint64_t{0x80}, std::uint64_t{0xff},
		std::uint64_t{0x7fff}, std::uint64_t{0x8000},
		std::uint64_t{0xffff}, std::uint64_t{0x7fff'ffffU},
		std::uint64_t{0x8000'0000U}, std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL}};
	for (const auto width : widths)
		for (const auto cc : valid_conditions)
			for (const auto left : values)
				for (const auto right : values) {
					auto initial = test::state();
					initial.registers[1] = left;
					initial.registers[2] = right;
					harness.check(instruction(width, cc, 2, 0x100, 1),
						initial, takes(width, cc, left, right), -1024);
				}
}

void aliases()
{
	/* CBLE/CBLO/CBLS/CBLT and their byte/halfword forms reverse the
	 * assembler operands and reuse GE/HI/HS/GT respectively.
	 */
	constexpr std::array values{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0x7f},
		std::uint64_t{0x80}, std::uint64_t{0xffff},
		std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffff'ffff'ffffULL}};
	for (const auto width : widths)
		for (const auto first : values)
			for (const auto second : values) {
				test::require(takes(width, 1, first, second) ==
					(signed_value(second, width) <=
					 signed_value(first, width)));
				test::require(takes(width, 2, first, second) ==
					((second & width_mask(width)) <
					 (first & width_mask(width))));
				test::require(takes(width, 3, first, second) ==
					((second & width_mask(width)) <=
					 (first & width_mask(width))));
				test::require(takes(width, 0, first, second) ==
					(signed_value(second, width) <
					 signed_value(first, width)));
			}
}

void reserved_encodings(test::Harness &harness)
{
	for (const auto width : widths)
		for (const unsigned cc : {4U, 5U})
			harness.check_illegal(instruction(width, cc, 2, 0x155, 1));
	/* register control 01 is reserved; narrow forms require sf == 0. */
	harness.check_illegal(0x7400'4000U | (2U << 16) | 1U);
	harness.check_illegal(instruction(8, 0, 2, 3, 1) | 0x8000'0000U);
	harness.check_illegal(instruction(16, 7, 31, 0x1ff, 31) |
		0x8000'0000U);
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	std::set<std::uintptr_t> constants;
	for (const auto width : widths)
		for (const auto cc : valid_conditions)
			for (unsigned mode = 0; mode < 3; ++mode) {
				const unsigned rt = mode == 2 ? 31U : 1U;
				const unsigned rm = mode == 1 ? 31U : 2U;
				const auto built = gadgets::Builder{}.build(test::base,
					std::array{instruction(width, cc, rm, 5, rt)});
				test::require(built.complete &&
					built.program.words().size() == 4);
				entries.insert(built.program.words().front());
			}
	for (const auto width : widths)
		for (const auto cc : valid_conditions) {
			const auto folded = gadgets::Builder{}.build(test::base,
				std::array{instruction(width, cc, 3, 5, 3)});
			test::require(folded.complete &&
				folded.program.words().size() == 2);
			constants.insert(folded.program.words().front());
		}
	test::require(entries.size() == 72 && constants.size() == 1);
}

} // namespace

int main()
{
	encoding_examples();
	test::Harness harness;
	exhaustive_fields(harness);
	signed_unsigned_boundaries(harness);
	aliases();
	reserved_encodings(harness);
	production_specialization();
	harness.print_summary("arm64-base-cmpbr-register");
	return 0;
}
