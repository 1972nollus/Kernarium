// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_absolute_minmax_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

namespace test = arm64_floating_absolute_minmax_test;

namespace {

namespace gadgets = uml::backend::arm64::gadgets;

extern "C" {
#define DECLARE_VECTOR(name) \
	void uml_arm64_gadget_floating_horizontal_##name##_4h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_8h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_2s(); \
	void uml_arm64_gadget_floating_horizontal_##name##_4s(); \
	void uml_arm64_gadget_floating_horizontal_##name##_2d();
DECLARE_VECTOR(famax_native)
DECLARE_VECTOR(famin_native)
#undef DECLARE_VECTOR
void uml_arm64_gadget_floating_horizontal_absolute_software();
}

template<typename Function>
[[nodiscard]] std::uintptr_t address(Function function)
{
	return reinterpret_cast<std::uintptr_t>(function);
}

[[nodiscard]] std::uintptr_t native_entry(const test::Form &form)
{
#define VECTOR(name) std::array<std::uintptr_t, 5>{ \
	address(uml_arm64_gadget_floating_horizontal_##name##_4h), \
	address(uml_arm64_gadget_floating_horizontal_##name##_8h), \
	address(uml_arm64_gadget_floating_horizontal_##name##_2s), \
	address(uml_arm64_gadget_floating_horizontal_##name##_4s), \
	address(uml_arm64_gadget_floating_horizontal_##name##_2d)}
	static const std::array table{
		VECTOR(famax_native), VECTOR(famin_native)};
#undef VECTOR
	return table[form.operation == test::Operation::absolute_minimum]
		[test::horizontal::vector_shape(
			form.width, form.vector_bytes)];
}

[[nodiscard]] std::uintptr_t vector_offset(unsigned reg)
{
	return offsetof(test::arm64::CpuState, vectors) +
		reg * sizeof(test::arm64::VectorRegister);
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
	for (unsigned index = 0; index < 24U; ++index) {
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

void translation_selection_and_native_hot_shapes()
{
	const bool native = test::engine_tests::host_supports(
		test::engine_tests::NativeRequirements::features(
			{uml::backend::ArmFeature::advanced_simd,
			 uml::backend::ArmFeature::faminmax}));
	std::set<std::uintptr_t> native_symbols;
	test::for_each_form([&](const test::Form &form) {
		const auto expected_native = native_entry(form);
		native_symbols.insert(expected_native);
		for (const auto registers : test::register_cases) {
			const auto encoded = test::instruction(form, registers);
			const auto built = gadgets::Builder{}.build(
				test::engine_tests::engine_test_code_base,
				std::array{encoded});
			test::common::require(
				built.complete && built.program.words().size() >= 2U);
			if (native) {
				test::common::require(
					built.program.words()[0] == expected_native);
				test::common::require(
					built.program.words().size() >= 4U);
				test::common::require(built.program.words()[1] ==
					vector_offset(registers.destination));
				test::common::require(built.program.words()[2] ==
					vector_offset(registers.first));
				test::common::require(built.program.words()[3] ==
					vector_offset(registers.second));
			} else {
				test::common::require(built.program.words()[0] ==
					address(
						uml_arm64_gadget_floating_horizontal_absolute_software));
				test::common::require(built.program.words()[1] ==
					test::horizontal::detail::
						floating_horizontal_operand(
							encoded,
							test::horizontal::detail::
								decode_floating_horizontal(
									encoded)));
			}
		}
	});
	test::common::require(native_symbols.size() == 10U);
	for (const auto entry : native_symbols) {
		const auto shape = body_shape(entry);
		test::common::require(shape.terminal);
		test::common::require(shape.conditional_branches <= 1U);
		test::common::require(shape.instructions <= 14U);
	}
}

} // namespace

int main()
{
	translation_selection_and_native_hot_shapes();
}
