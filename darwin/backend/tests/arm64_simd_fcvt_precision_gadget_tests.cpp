// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvt_precision_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>

extern "C" {
void uml_arm64_gadget_floating_convert_h_to_s();
void uml_arm64_gadget_floating_convert_h_to_d();
void uml_arm64_gadget_floating_convert_s_to_h();
void uml_arm64_gadget_floating_convert_s_to_d();
void uml_arm64_gadget_floating_convert_d_to_h();
void uml_arm64_gadget_floating_convert_d_to_s();
void uml_arm64_gadget_illegal();
}

namespace test = arm64_simd_fcvt_precision_test;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

using Gadget = void (*)();

constexpr std::array<Gadget, 6> declared_gadgets{
	uml_arm64_gadget_floating_convert_h_to_s,
	uml_arm64_gadget_floating_convert_h_to_d,
	uml_arm64_gadget_floating_convert_s_to_h,
	uml_arm64_gadget_floating_convert_s_to_d,
	uml_arm64_gadget_floating_convert_d_to_h,
	uml_arm64_gadget_floating_convert_d_to_s,
};

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

[[nodiscard]] std::uintptr_t address(Gadget gadget)
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] constexpr std::uintptr_t vector_offset(unsigned reg)
{
	return offsetof(test::arm64::CpuState, vectors) +
		reg * sizeof(test::arm64::VectorRegister);
}

[[nodiscard]] constexpr std::uintptr_t translated_operand(
	test::Registers registers)
{
	static_assert(vector_offset(31U) <= 0xffffU);
	return vector_offset(registers.destination) |
		(vector_offset(registers.source) << 16U);
}

void translation_selects_each_precision_pair_and_direct_offsets()
{
	constexpr auto base =
		test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U};
	constexpr std::array tail{std::uint32_t{0xd400'0001U}};
	std::set<std::uintptr_t> selected;
	for (unsigned index = 0U; index < test::pairs.size(); ++index) {
		const std::array code{
			test::instruction(test::pairs[index], registers), tail[0]};
		const auto built = gadgets::Builder{}.build(base, code);
		require(built.complete,
			"valid scalar precision conversion did not build");
		require(built.program.instruction_count() == 2U,
			"scalar precision conversion changed block length");
		require(built.program.accesses_fp_state(),
			"scalar precision conversion lost FP-state tracking");
		const auto words = built.program.words();
		require(words.size() >= 2U,
			"scalar precision gadget stream is truncated");
		require(words[0] == address(declared_gadgets[index]),
			"precision pair remained undecoded in the runtime stream");
		require(words[1] == translated_operand(registers),
			"register fields were not translated to direct offsets");
		selected.insert(words[0]);
	}
	std::set<std::uintptr_t> declared;
	for (const auto gadget : declared_gadgets)
		declared.insert(address(gadget));
	require(declared.size() == declared_gadgets.size(),
		"scalar precision gadget table contains duplicate entries");
	require(selected == declared,
		"scalar precision gadget table contains an unused entry");
}

void reserved_pairs_translate_directly_to_illegal()
{
	constexpr auto base =
		test::engine_tests::engine_test_code_base;
	constexpr std::array tail{std::uint32_t{0xd400'0001U}};
	for (unsigned source_type = 0U; source_type < 4U; ++source_type)
		for (unsigned destination_type = 0U;
		     destination_type < 4U; ++destination_type) {
			if (test::valid_type_pair(source_type, destination_type) ||
			    test::belongs_to_bfloat_conversion(source_type,
				    destination_type))
				continue;
			const std::array code{
				test::instruction_from_types(source_type,
					destination_type, {31U, 30U}),
				tail[0]};
			const auto built = gadgets::Builder{}.build(base, code);
			require(built.complete,
				"reserved scalar precision conversion did not build");
			const auto words = built.program.words();
			require(!words.empty() &&
					words[0] ==
						address(uml_arm64_gadget_illegal),
				"reserved precision pair reached a runtime decoder");
		}
}

} // namespace

int main()
{
	translation_selects_each_precision_pair_and_direct_offsets();
	reserved_pairs_translate_directly_to_illegal();
	return 0;
}
