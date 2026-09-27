// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fused_floating_test_support.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

namespace test = arm64_fused_floating_test;

namespace {

namespace gadgets = uml::cpu::arm64::gadgets;
namespace arm64 = uml::cpu::arm64;

extern "C" {
#define DECLARE_SCALAR(operation) \
	void uml_arm64_gadget_floating_##operation##_half(); \
	void uml_arm64_gadget_floating_##operation##_single(); \
	void uml_arm64_gadget_floating_##operation##_double();
DECLARE_SCALAR(madd)
DECLARE_SCALAR(msub)
DECLARE_SCALAR(nmadd)
DECLARE_SCALAR(nmsub)
#undef DECLARE_SCALAR
}

template<typename Function>
[[nodiscard]] std::uintptr_t address(Function function)
{
	return reinterpret_cast<std::uintptr_t>(function);
}

[[nodiscard]] std::uintptr_t scalar_entry(const test::Form &form)
{
	const unsigned width = form.width == 16U ? 0U :
		form.width == 32U ? 1U : 2U;
#define SCALAR_ROW(operation) std::array<std::uintptr_t, 3>{ \
	address(uml_arm64_gadget_floating_##operation##_half), \
	address(uml_arm64_gadget_floating_##operation##_single), \
	address(uml_arm64_gadget_floating_##operation##_double)}
	static const std::array table{
		SCALAR_ROW(madd), SCALAR_ROW(msub),
		SCALAR_ROW(nmadd), SCALAR_ROW(nmsub),
	};
#undef SCALAR_ROW
	const unsigned operation = form.negate * 2U + form.subtract;
	return table[operation][width];
}

/* Every vector FMLA/FMLS accumulates into Vd, so the translator always
 * reaches the baked-accumulator entry for that register. */
[[nodiscard]] std::uintptr_t vector_entry(const test::Form &form,
	unsigned destination)
{
	const unsigned shape = form.width == 16U ?
		(form.vector_bytes == 16U ? 1U : 0U) :
		form.width == 32U ?
		(form.vector_bytes == 16U ? 3U : 2U) : 4U;
	const unsigned row = (form.subtract ? 5U : 0U) + shape;
	return reinterpret_cast<std::uintptr_t>(
		gadgets::selection::uml_arm64_gadget_simd_fused_inplace_table[
			row * 32U + destination]);
}

[[nodiscard]] std::uintptr_t vector_offset(unsigned reg)
{
	return offsetof(arm64::CpuState, vectors) +
		reg * sizeof(arm64::VectorRegister);
}

struct BodyShape final {
	unsigned instructions{};
	unsigned conditional_branches{};
	bool terminal{};
};

[[nodiscard]] BodyShape body_shape(std::uintptr_t entry)
{
	const auto *code = reinterpret_cast<const std::uint32_t *>(entry);
	BodyShape result{};
	for (unsigned index = 0; index < 32U; ++index) {
		const auto instruction = code[index];
		if ((instruction & 0xffff'fc1fU) == 0xd61f'0000U) {
			result.terminal = true;
			break;
		}
		if ((instruction & 0xff00'0010U) == 0x5400'0000U ||
		    (instruction & 0x7e00'0000U) == 0x3400'0000U ||
		    (instruction & 0x7e00'0000U) == 0x3600'0000U)
			++result.conditional_branches;
		++result.instructions;
	}
	return result;
}

void direct_operands_and_live_bank()
{
	std::set<std::uintptr_t> selected;
	test::for_each_form([&](const test::Form &form) {
		for (const auto registers : test::register_cases) {
			const auto built = gadgets::Builder{}.build(
				test::engine_tests::engine_test_code_base,
				std::array{test::instruction(form, registers)});
			test::common::require(
				built.complete && !built.program.words().empty());
			const auto expected_entry = form.scalar ?
				scalar_entry(form) :
				vector_entry(form, registers.destination);
			test::common::require(
				built.program.words()[0] == expected_entry);
			test::common::require(
				built.program.words().size() == 4U);
			test::common::require(built.program.words()[1] ==
				gadgets::selection::packed_u16_operands(
					vector_offset(registers.destination),
					vector_offset(registers.left),
					vector_offset(registers.right),
					form.scalar ?
						vector_offset(registers.addend) : 0U));
			selected.insert(expected_entry);
		}
	});

	std::set<std::uintptr_t> expected;
	test::for_each_form([&](const test::Form &form) {
		if (form.scalar) {
			expected.insert(scalar_entry(form));
			return;
		}
		/* A vector form reaches one entry per destination register when
		 * the accumulator displacement is baked, and one entry in all
		 * for the established bank. */
		for (const auto registers : test::register_cases)
			expected.insert(vector_entry(form, registers.destination));
	});
	test::common::require(selected == expected);

	for (const auto entry : expected) {
		const auto shape = body_shape(entry);
		test::common::require(shape.terminal);
		const bool scalar =
			entry == address(uml_arm64_gadget_floating_madd_half) ||
			entry == address(uml_arm64_gadget_floating_madd_single) ||
			entry == address(uml_arm64_gadget_floating_madd_double) ||
			entry == address(uml_arm64_gadget_floating_msub_half) ||
			entry == address(uml_arm64_gadget_floating_msub_single) ||
			entry == address(uml_arm64_gadget_floating_msub_double) ||
			entry == address(uml_arm64_gadget_floating_nmadd_half) ||
			entry == address(uml_arm64_gadget_floating_nmadd_single) ||
			entry == address(uml_arm64_gadget_floating_nmadd_double) ||
			entry == address(uml_arm64_gadget_floating_nmsub_half) ||
			entry == address(uml_arm64_gadget_floating_nmsub_single) ||
			entry == address(uml_arm64_gadget_floating_nmsub_double);
		test::common::require(shape.conditional_branches <=
			(scalar ? 2U : 1U));
		test::common::require(shape.instructions <=
			(scalar ? 25U : 16U));
	}
}

} // namespace

int main()
{
	direct_operands_and_live_bank();
}
