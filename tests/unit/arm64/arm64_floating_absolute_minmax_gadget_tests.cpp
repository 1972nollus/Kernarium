// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_floating_absolute_minmax_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

namespace a64 = uml::testing::a64;

namespace test = arm64_floating_absolute_minmax_test;

namespace {

namespace gadgets = uml::cpu::arm64::gadgets;

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

void translation_selection_and_native_hot_shapes()
{
	const bool native = test::engine_tests::host_supports(
		test::engine_tests::NativeRequirements::features(
			{::uml::cpu::ArmFeature::advanced_simd,
			 ::uml::cpu::ArmFeature::faminmax}));
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
		const auto shape = a64::body_shape(entry, {.window = 24U});
		test::common::require(shape.reached_dispatch);
		test::common::require(shape.conditional_branches <= 1U);
		test::common::require(shape.instructions <= 14U);
	}
}

} // namespace

int main()
{
	translation_selection_and_native_hot_shapes();
}
