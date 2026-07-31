// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_rearrange_test_support.hpp"
#include "arm64_simd_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

extern "C" {
void uml_arm64_gadget_svc();
#define UML_DECLARE_PERMUTE(name) \
	void uml_arm64_gadget_simd_permute_##name##_8b(); \
	void uml_arm64_gadget_simd_permute_##name##_4h(); \
	void uml_arm64_gadget_simd_permute_##name##_2s(); \
	void uml_arm64_gadget_simd_permute_##name##_16b(); \
	void uml_arm64_gadget_simd_permute_##name##_8h(); \
	void uml_arm64_gadget_simd_permute_##name##_4s(); \
	void uml_arm64_gadget_simd_permute_##name##_2d();
UML_DECLARE_PERMUTE(uzp1)
UML_DECLARE_PERMUTE(trn1)
UML_DECLARE_PERMUTE(zip1)
UML_DECLARE_PERMUTE(uzp2)
UML_DECLARE_PERMUTE(trn2)
UML_DECLARE_PERMUTE(zip2)
#undef UML_DECLARE_PERMUTE
#define UML_DECLARE_TABLE(operation, count) \
	void uml_arm64_gadget_simd_##operation##_##count##_8b(); \
	void uml_arm64_gadget_simd_##operation##_##count##_16b();
UML_DECLARE_TABLE(tbl, 1)
UML_DECLARE_TABLE(tbl, 2)
UML_DECLARE_TABLE(tbl, 3)
UML_DECLARE_TABLE(tbl, 4)
UML_DECLARE_TABLE(tbx, 1)
UML_DECLARE_TABLE(tbx, 2)
UML_DECLARE_TABLE(tbx, 3)
UML_DECLARE_TABLE(tbx, 4)
#undef UML_DECLARE_TABLE
}

namespace test = arm64_simd_rearrange_test;
namespace base = arm64_simd_test;
namespace gadgets = uml::backend::arm64::gadgets;
namespace arm64 = uml::backend::arm64;

namespace {

using Gadget = void (*)();

#define UML_PERMUTE_SET(name) std::array<Gadget, 7>{ \
	uml_arm64_gadget_simd_permute_##name##_8b, \
	uml_arm64_gadget_simd_permute_##name##_4h, \
	uml_arm64_gadget_simd_permute_##name##_2s, \
	uml_arm64_gadget_simd_permute_##name##_16b, \
	uml_arm64_gadget_simd_permute_##name##_8h, \
	uml_arm64_gadget_simd_permute_##name##_4s, \
	uml_arm64_gadget_simd_permute_##name##_2d}

struct ExpectedPermute final {
	unsigned opcode;
	std::array<Gadget, 7> shapes;
};

constexpr std::array expected_permutes{
	ExpectedPermute{1U, UML_PERMUTE_SET(uzp1)},
	ExpectedPermute{2U, UML_PERMUTE_SET(trn1)},
	ExpectedPermute{3U, UML_PERMUTE_SET(zip1)},
	ExpectedPermute{5U, UML_PERMUTE_SET(uzp2)},
	ExpectedPermute{6U, UML_PERMUTE_SET(trn2)},
	ExpectedPermute{7U, UML_PERMUTE_SET(zip2)},
};
#undef UML_PERMUTE_SET

constexpr std::array<Gadget, 16> expected_tables{
	uml_arm64_gadget_simd_tbl_1_8b,
	uml_arm64_gadget_simd_tbl_1_16b,
	uml_arm64_gadget_simd_tbl_2_8b,
	uml_arm64_gadget_simd_tbl_2_16b,
	uml_arm64_gadget_simd_tbl_3_8b,
	uml_arm64_gadget_simd_tbl_3_16b,
	uml_arm64_gadget_simd_tbl_4_8b,
	uml_arm64_gadget_simd_tbl_4_16b,
	uml_arm64_gadget_simd_tbx_1_8b,
	uml_arm64_gadget_simd_tbx_1_16b,
	uml_arm64_gadget_simd_tbx_2_8b,
	uml_arm64_gadget_simd_tbx_2_16b,
	uml_arm64_gadget_simd_tbx_3_8b,
	uml_arm64_gadget_simd_tbx_3_16b,
	uml_arm64_gadget_simd_tbx_4_8b,
	uml_arm64_gadget_simd_tbx_4_16b,
};

[[nodiscard]] std::uintptr_t address_of(Gadget gadget)
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] constexpr std::uintptr_t vector_offset(unsigned index)
{
	return offsetof(arm64::CpuState, vectors) +
		index * sizeof(arm64::VectorRegister);
}

static_assert(vector_offset(31U) <= 0xffffU);

[[nodiscard]] gadgets::Program build_one(std::uint32_t instruction)
{
	const std::array stream{instruction, 0xd400'0001U};
	auto built = gadgets::Builder{}.build(
		test::engine_tests::engine_test_code_base, stream);
	base::require(built.complete);
	return std::move(built.program);
}

void every_operation_and_shape_is_selected_during_translation()
{
	constexpr test::PermuteRegisters registers{31U, 30U, 29U};
	const auto operand =
		vector_offset(registers.destination) |
		(vector_offset(registers.left) << 16) |
		(vector_offset(registers.right) << 32);
	for (const auto &operation : expected_permutes)
		for (const bool q : {false, true})
			for (unsigned size = 0; size < (q ? 4U : 3U); ++size) {
				const auto program = build_one(test::permute_instruction(
					q, size, operation.opcode, registers));
				const unsigned shape = q ? size + 3U : size;
				base::require(program.words()[0] ==
					address_of(operation.shapes[shape]));
				base::require(program.words()[1] == operand);
				base::require(program.words()[2] ==
					address_of(uml_arm64_gadget_svc));
			}
}

void every_table_form_has_resolved_wrapped_offsets()
{
	constexpr test::TableRegisters registers{31U, 31U, 30U};
	const auto first_operand =
		vector_offset(registers.destination) |
		(vector_offset(registers.indices) << 16) |
		(vector_offset(31U) << 32) |
		(vector_offset(0U) << 48);
	const auto remaining_operand =
		vector_offset(1U) | (vector_offset(2U) << 16);
	for (const bool extend : {false, true})
		for (unsigned count = 1; count <= 4U; ++count)
			for (const bool q : {false, true}) {
				const auto program = build_one(test::table_instruction(
					q, extend, count, registers));
				const unsigned shape = (extend ? 8U : 0U) +
					(count - 1U) * 2U + (q ? 1U : 0U);
				base::require(program.words()[0] ==
					address_of(expected_tables[shape]));
				base::require(program.words()[1] == first_operand);
				if (count > 2U) {
					base::require(
						program.words()[2] == remaining_operand);
					base::require(program.words()[3] ==
						address_of(uml_arm64_gadget_svc));
				} else {
					base::require(program.words()[2] ==
						address_of(uml_arm64_gadget_svc));
				}
			}
}

[[nodiscard]] constexpr bool conditional_branch(std::uint32_t instruction)
{
	return (instruction & 0xff00'0010U) == 0x5400'0000U ||
		(instruction & 0x7e00'0000U) == 0x3400'0000U ||
		(instruction & 0x7e00'0000U) == 0x3600'0000U;
}

[[nodiscard]] constexpr bool indirect_branch(std::uint32_t instruction)
{
	return (instruction & 0xffff'fc1fU) == 0xd61f'0000U;
}

void require_branch_free(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(gadget);
	bool reached_dispatch = false;
	for (unsigned index = 0; index < 64U; ++index) {
		base::require(!conditional_branch(instructions[index]));
		if (indirect_branch(instructions[index])) {
			reached_dispatch = true;
			break;
		}
	}
	base::require(reached_dispatch);
}

void every_specialized_hot_path_is_branch_free()
{
	for (const auto &operation : expected_permutes)
		for (const auto gadget : operation.shapes)
			require_branch_free(gadget);
	for (const auto gadget : expected_tables)
		require_branch_free(gadget);
}

} // namespace

int main()
{
	every_operation_and_shape_is_selected_during_translation();
	every_table_form_has_resolved_wrapped_offsets();
	every_specialized_hot_path_is_branch_free();
	return 0;
}
