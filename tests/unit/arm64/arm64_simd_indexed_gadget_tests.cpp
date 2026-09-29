// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_simd_indexed_test_support.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

namespace a64 = uml::testing::a64;

extern "C" {
#define UML_DECLARE_INDEXED_ONE(name, shape, index) \
	void uml_arm64_gadget_simd_indexed_##name##_##shape##_##index();
#define UML_DECLARE_INDEXED_4(name, shape) \
	UML_DECLARE_INDEXED_ONE(name, shape, 0) \
	UML_DECLARE_INDEXED_ONE(name, shape, 1) \
	UML_DECLARE_INDEXED_ONE(name, shape, 2) \
	UML_DECLARE_INDEXED_ONE(name, shape, 3)
#define UML_DECLARE_INDEXED_8(name, shape) \
	UML_DECLARE_INDEXED_4(name, shape) \
	UML_DECLARE_INDEXED_ONE(name, shape, 4) \
	UML_DECLARE_INDEXED_ONE(name, shape, 5) \
	UML_DECLARE_INDEXED_ONE(name, shape, 6) \
	UML_DECLARE_INDEXED_ONE(name, shape, 7)
#define UML_DECLARE_INDEXED_SAME(name) \
	UML_DECLARE_INDEXED_8(name, 4h) \
	UML_DECLARE_INDEXED_8(name, 8h) \
	UML_DECLARE_INDEXED_4(name, 2s) \
	UML_DECLARE_INDEXED_4(name, 4s)
#define UML_DECLARE_INDEXED_LONG(name) \
	UML_DECLARE_INDEXED_8(name, h_low) \
	UML_DECLARE_INDEXED_8(name, h_high) \
	UML_DECLARE_INDEXED_4(name, s_low) \
	UML_DECLARE_INDEXED_4(name, s_high)
#define UML_DECLARE_INDEXED_SATURATING(name) \
	UML_DECLARE_INDEXED_8(name, h_low) \
	UML_DECLARE_INDEXED_8(name, h_high) \
	UML_DECLARE_INDEXED_8(name, h_scalar) \
	UML_DECLARE_INDEXED_4(name, s_low) \
	UML_DECLARE_INDEXED_4(name, s_high) \
	UML_DECLARE_INDEXED_4(name, s_scalar)
UML_DECLARE_INDEXED_SAME(mla)
UML_DECLARE_INDEXED_SAME(mls)
UML_DECLARE_INDEXED_SAME(mul)
UML_DECLARE_INDEXED_LONG(smlal)
UML_DECLARE_INDEXED_LONG(smlsl)
UML_DECLARE_INDEXED_LONG(smull)
UML_DECLARE_INDEXED_LONG(umlal)
UML_DECLARE_INDEXED_LONG(umlsl)
UML_DECLARE_INDEXED_LONG(umull)
UML_DECLARE_INDEXED_SATURATING(sqdmlal)
UML_DECLARE_INDEXED_SATURATING(sqdmlsl)
UML_DECLARE_INDEXED_SATURATING(sqdmull)
UML_DECLARE_INDEXED_SATURATING(sqdmulh)
UML_DECLARE_INDEXED_SATURATING(sqrdmulh)
UML_DECLARE_INDEXED_SATURATING(sqrdmlah)
UML_DECLARE_INDEXED_SATURATING(sqrdmlsh)
#undef UML_DECLARE_INDEXED_SATURATING
#undef UML_DECLARE_INDEXED_LONG
#undef UML_DECLARE_INDEXED_SAME
#undef UML_DECLARE_INDEXED_8
#undef UML_DECLARE_INDEXED_4
#undef UML_DECLARE_INDEXED_ONE

#define UML_DECLARE_WIDENING(name) \
	void uml_arm64_gadget_simd_widening_##name##_8b(); \
	void uml_arm64_gadget_simd_widening_##name##_4h(); \
	void uml_arm64_gadget_simd_widening_##name##_2s(); \
	void uml_arm64_gadget_simd_widening_##name##_16b(); \
	void uml_arm64_gadget_simd_widening_##name##_8h(); \
	void uml_arm64_gadget_simd_widening_##name##_4s();
#define UML_DECLARE_SATURATING_LONG(name) \
	void uml_arm64_gadget_simd_widening_##name##_4h(); \
	void uml_arm64_gadget_simd_widening_##name##_2s(); \
	void uml_arm64_gadget_simd_widening_##name##_8h(); \
	void uml_arm64_gadget_simd_widening_##name##_4s(); \
	void uml_arm64_gadget_simd_widening_##name##_h(); \
	void uml_arm64_gadget_simd_widening_##name##_s();
UML_DECLARE_WIDENING(smlal)
UML_DECLARE_WIDENING(smlsl)
UML_DECLARE_WIDENING(smull)
UML_DECLARE_WIDENING(umlal)
UML_DECLARE_WIDENING(umlsl)
UML_DECLARE_WIDENING(umull)
UML_DECLARE_SATURATING_LONG(sqdmlal)
UML_DECLARE_SATURATING_LONG(sqdmlsl)
UML_DECLARE_SATURATING_LONG(sqdmull)
#undef UML_DECLARE_SATURATING_LONG
#undef UML_DECLARE_WIDENING

#define UML_DECLARE_RDM(name) \
	void uml_arm64_gadget_simd_three_same_##name##_4h(); \
	void uml_arm64_gadget_simd_three_same_##name##_2s(); \
	void uml_arm64_gadget_simd_three_same_##name##_8h(); \
	void uml_arm64_gadget_simd_three_same_##name##_4s(); \
	void uml_arm64_gadget_simd_three_same_##name##_h(); \
	void uml_arm64_gadget_simd_three_same_##name##_s();
UML_DECLARE_RDM(sqrdmlah)
UML_DECLARE_RDM(sqrdmlsh)
#undef UML_DECLARE_RDM
void uml_arm64_gadget_simd_rdm_software();
std::uint64_t uml_arm64_simd_rdm_software(
	uml::cpu::arm64::CpuState *, std::uint32_t) noexcept;
}

namespace test = arm64_simd_indexed_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using Gadget = void (*)();

[[nodiscard]] std::uintptr_t address(Gadget gadget)
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] constexpr std::uintptr_t vector_offset(unsigned reg)
{
	return offsetof(test::arm64::CpuState, vectors) +
		reg * sizeof(test::arm64::VectorRegister);
}

#define UML_ADD_INDEXED_ONE(set, name, shape, index) \
	(set).insert(address( \
		uml_arm64_gadget_simd_indexed_##name##_##shape##_##index));
#define UML_ADD_INDEXED_4(set, name, shape) \
	UML_ADD_INDEXED_ONE(set, name, shape, 0) \
	UML_ADD_INDEXED_ONE(set, name, shape, 1) \
	UML_ADD_INDEXED_ONE(set, name, shape, 2) \
	UML_ADD_INDEXED_ONE(set, name, shape, 3)
#define UML_ADD_INDEXED_8(set, name, shape) \
	UML_ADD_INDEXED_4(set, name, shape) \
	UML_ADD_INDEXED_ONE(set, name, shape, 4) \
	UML_ADD_INDEXED_ONE(set, name, shape, 5) \
	UML_ADD_INDEXED_ONE(set, name, shape, 6) \
	UML_ADD_INDEXED_ONE(set, name, shape, 7)
#define UML_ADD_INDEXED_SAME(set, name) \
	UML_ADD_INDEXED_8(set, name, 4h) \
	UML_ADD_INDEXED_8(set, name, 8h) \
	UML_ADD_INDEXED_4(set, name, 2s) \
	UML_ADD_INDEXED_4(set, name, 4s)
#define UML_ADD_INDEXED_LONG(set, name) \
	UML_ADD_INDEXED_8(set, name, h_low) \
	UML_ADD_INDEXED_8(set, name, h_high) \
	UML_ADD_INDEXED_4(set, name, s_low) \
	UML_ADD_INDEXED_4(set, name, s_high)
#define UML_ADD_INDEXED_SATURATING(set, name) \
	UML_ADD_INDEXED_8(set, name, h_low) \
	UML_ADD_INDEXED_8(set, name, h_high) \
	UML_ADD_INDEXED_8(set, name, h_scalar) \
	UML_ADD_INDEXED_4(set, name, s_low) \
	UML_ADD_INDEXED_4(set, name, s_high) \
	UML_ADD_INDEXED_4(set, name, s_scalar)

#define UML_ADD_WIDENING(set, name) \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_8b)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_4h)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_2s)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_16b)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_8h)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_4s));
#define UML_ADD_SATURATING_LONG(set, name) \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_4h)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_2s)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_8h)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_4s)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_h)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_s));
#define UML_ADD_RDM(set, name) \
	(set).insert(address(uml_arm64_gadget_simd_three_same_##name##_4h)); \
	(set).insert(address(uml_arm64_gadget_simd_three_same_##name##_2s)); \
	(set).insert(address(uml_arm64_gadget_simd_three_same_##name##_8h)); \
	(set).insert(address(uml_arm64_gadget_simd_three_same_##name##_4s)); \
	(set).insert(address(uml_arm64_gadget_simd_three_same_##name##_h)); \
	(set).insert(address(uml_arm64_gadget_simd_three_same_##name##_s));

void add_non_rdm_entries(std::set<std::uintptr_t> &expected,
	std::set<std::uintptr_t> &saturating)
{
	UML_ADD_INDEXED_SAME(expected, mla)
	UML_ADD_INDEXED_SAME(expected, mls)
	UML_ADD_INDEXED_SAME(expected, mul)
	UML_ADD_INDEXED_LONG(expected, smlal)
	UML_ADD_INDEXED_LONG(expected, smlsl)
	UML_ADD_INDEXED_LONG(expected, smull)
	UML_ADD_INDEXED_LONG(expected, umlal)
	UML_ADD_INDEXED_LONG(expected, umlsl)
	UML_ADD_INDEXED_LONG(expected, umull)
	UML_ADD_WIDENING(expected, smlal)
	UML_ADD_WIDENING(expected, smlsl)
	UML_ADD_WIDENING(expected, smull)
	UML_ADD_WIDENING(expected, umlal)
	UML_ADD_WIDENING(expected, umlsl)
	UML_ADD_WIDENING(expected, umull)

	UML_ADD_INDEXED_SATURATING(expected, sqdmlal)
	UML_ADD_INDEXED_SATURATING(expected, sqdmlsl)
	UML_ADD_INDEXED_SATURATING(expected, sqdmull)
	UML_ADD_INDEXED_SATURATING(expected, sqdmulh)
	UML_ADD_INDEXED_SATURATING(expected, sqrdmulh)
	UML_ADD_SATURATING_LONG(expected, sqdmlal)
	UML_ADD_SATURATING_LONG(expected, sqdmlsl)
	UML_ADD_SATURATING_LONG(expected, sqdmull)
	UML_ADD_INDEXED_SATURATING(saturating, sqdmlal)
	UML_ADD_INDEXED_SATURATING(saturating, sqdmlsl)
	UML_ADD_INDEXED_SATURATING(saturating, sqdmull)
	UML_ADD_INDEXED_SATURATING(saturating, sqdmulh)
	UML_ADD_INDEXED_SATURATING(saturating, sqrdmulh)
	UML_ADD_SATURATING_LONG(saturating, sqdmlal)
	UML_ADD_SATURATING_LONG(saturating, sqdmlsl)
	UML_ADD_SATURATING_LONG(saturating, sqdmull)
}

void add_rdm_entries(std::set<std::uintptr_t> &expected,
	std::set<std::uintptr_t> &saturating)
{
	UML_ADD_INDEXED_SATURATING(expected, sqrdmlah)
	UML_ADD_INDEXED_SATURATING(expected, sqrdmlsh)
	UML_ADD_RDM(expected, sqrdmlah)
	UML_ADD_RDM(expected, sqrdmlsh)
	UML_ADD_INDEXED_SATURATING(saturating, sqrdmlah)
	UML_ADD_INDEXED_SATURATING(saturating, sqrdmlsh)
	UML_ADD_RDM(saturating, sqrdmlah)
	UML_ADD_RDM(saturating, sqrdmlsh)
}

#undef UML_ADD_RDM
#undef UML_ADD_SATURATING_LONG
#undef UML_ADD_WIDENING
#undef UML_ADD_INDEXED_SATURATING
#undef UML_ADD_INDEXED_LONG
#undef UML_ADD_INDEXED_SAME
#undef UML_ADD_INDEXED_8
#undef UML_ADD_INDEXED_4
#undef UML_ADD_INDEXED_ONE

struct Body final {
	unsigned instructions{};
	unsigned conditional_branches{};
	unsigned calls{};
};

[[nodiscard]] Body body(std::uintptr_t entry)
{
	Body result{};
	const auto shape = a64::body_shape(entry, {.window = 64U},
		[&](std::uint32_t instruction) {
			result.calls += (instruction & 0xfc00'0000U) == 0x9400'0000U;
		});
	if (!shape.reached_dispatch)
		test::fail("multiply gadget did not reach indirect dispatch");
	result.instructions = shape.instructions + 1U;
	result.conditional_branches = shape.conditional_branches;
	return result;
}

void every_form_uses_direct_offsets_and_no_gadget_is_dead()
{
	const bool native_rdm = test::engine_tests::host_supports(
		test::engine_tests::NativeRequirements::features(
			{::uml::cpu::ArmFeature::advanced_simd,
			 ::uml::cpu::ArmFeature::rdm}));
	std::set<std::uintptr_t> expected;
	std::set<std::uintptr_t> saturating;
	add_non_rdm_entries(expected, saturating);
	if (native_rdm)
		add_rdm_entries(expected, saturating);
	else
		expected.insert(address(
			uml_arm64_gadget_simd_rdm_software));
	test::require(expected.size() == (native_rdm ? 534U : 451U),
		"multiply gadget inventory has unexpected size");
	test::require(saturating.size() == (native_rdm ? 282U : 198U),
		"non-RDM saturating gadget inventory has unexpected size");

	constexpr auto base =
		test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U, 15U, 0U};
	std::set<std::uintptr_t> selected;
	for (const auto family : test::all_families)
		for (const auto shape : test::allocated_shapes(family))
			for (unsigned index = 0;
			     index < test::lane_count(family, shape); ++index) {
				auto current = registers;
				current.right =
					shape.size == 1U && test::indexed(family) ?
						15U : 29U;
				current.index = index;
				const auto encoded =
					test::instruction(family, shape, current);
				const auto built = gadgets::Builder{}.build(
					base, std::array{encoded});
				test::require(built.complete,
					"allocated multiply instruction did not build");
				const auto words = built.program.words();
				const bool rdm = test::is_rdm(family.operation);
				if (rdm && !native_rdm) {
					test::require(words.size() >= 2U &&
						words[0] == address(
							uml_arm64_gadget_simd_rdm_software) &&
						words[1] == encoded,
						"non-RDM host did not select exact software helper");
				} else {
					test::require(words.size() == 4U,
						"multiply gadget stream has unexpected shape");
					test::require(words[1] ==
							gadgets::selection::packed_u16_operands(
								vector_offset(current.destination),
								vector_offset(current.left),
								vector_offset(current.right)),
						"register offsets were not packed during translation");
				}
				selected.insert(words[0]);
			}
	test::require(selected == expected,
		"multiply gadget bank contains dead or unselected entries");

	for (const auto entry : selected) {
		if (entry == address(uml_arm64_gadget_simd_rdm_software))
			continue;
		const auto shape = body(entry);
		const bool updates_fpsr = saturating.contains(entry);
		if (updates_fpsr) {
			test::require(shape.conditional_branches == 1U,
				"saturating hot path needs exactly one FP activation branch");
			test::require(shape.calls == 1U,
				"saturating hot path needs exactly one cold activation call");
			test::require(shape.instructions <= 16U,
				"saturating hot path exceeds instruction budget");
		} else {
			test::require(shape.conditional_branches == 0U,
				"ordinary multiply hot path contains a conditional branch");
			test::require(shape.calls == 0U,
				"ordinary multiply hot path contains a call");
			test::require(shape.instructions <= 11U,
				"ordinary multiply hot path exceeds instruction budget");
		}
	}
}

void corrupt_software_metadata_reports_failure_without_mutation()
{
	test::arm64::CpuState cpu{};
	cpu.fpsr = 0x8800'0011U;
	cpu.vectors[3].bytes.fill(std::byte{0x5a});
	const auto vectors = cpu.vectors;
	const auto fpsr = cpu.fpsr;
	test::require(uml_arm64_simd_rdm_software(
		&cpu, 0xffff'ffffU) == 0U,
		"invalid cold-helper metadata did not report failure");
	for (std::size_t index = 0; index < cpu.vectors.size(); ++index)
		test::require(cpu.vectors[index].bytes == vectors[index].bytes,
			"invalid cold-helper metadata changed a vector register");
	test::require(cpu.fpsr == fpsr,
		"invalid cold-helper metadata changed status state");
}

} // namespace

int main()
{
	every_form_uses_direct_offsets_and_no_gadget_is_dead();
	corrupt_software_metadata_reports_failure_without_mutation();
	return 0;
}
