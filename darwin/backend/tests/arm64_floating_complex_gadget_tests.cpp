// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_complex_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>

extern "C" {
#define UML_DECLARE_ROTATIONS(name) \
	void uml_arm64_gadget_floating_complex_##name##_0(); \
	void uml_arm64_gadget_floating_complex_##name##_90(); \
	void uml_arm64_gadget_floating_complex_##name##_180(); \
	void uml_arm64_gadget_floating_complex_##name##_270();
#define UML_DECLARE_ADD(shape) \
	void uml_arm64_gadget_floating_complex_fcadd_##shape##_90(); \
	void uml_arm64_gadget_floating_complex_fcadd_##shape##_270();
UML_DECLARE_ADD(4h)
UML_DECLARE_ADD(8h)
UML_DECLARE_ADD(2s)
UML_DECLARE_ADD(4s)
UML_DECLARE_ADD(2d)
UML_DECLARE_ROTATIONS(fcmla_4h)
UML_DECLARE_ROTATIONS(fcmla_8h)
UML_DECLARE_ROTATIONS(fcmla_2s)
UML_DECLARE_ROTATIONS(fcmla_4s)
UML_DECLARE_ROTATIONS(fcmla_2d)
UML_DECLARE_ROTATIONS(fcmla_element_4h_0)
UML_DECLARE_ROTATIONS(fcmla_element_4h_1)
UML_DECLARE_ROTATIONS(fcmla_element_8h_0)
UML_DECLARE_ROTATIONS(fcmla_element_8h_1)
UML_DECLARE_ROTATIONS(fcmla_element_8h_2)
UML_DECLARE_ROTATIONS(fcmla_element_8h_3)
UML_DECLARE_ROTATIONS(fcmla_element_4s_0)
UML_DECLARE_ROTATIONS(fcmla_element_4s_1)
#undef UML_DECLARE_ADD
#undef UML_DECLARE_ROTATIONS
}

namespace test = arm64_floating_complex_test;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

using Gadget = void (*)();

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

[[nodiscard]] Gadget expected_gadget(const test::Form &form)
{
#define UML_ADD_PAIR(shape) std::array<Gadget, 2>{ \
	uml_arm64_gadget_floating_complex_fcadd_##shape##_90, \
	uml_arm64_gadget_floating_complex_fcadd_##shape##_270}
	static constexpr std::array add{
		UML_ADD_PAIR(4h),
		UML_ADD_PAIR(8h),
		UML_ADD_PAIR(2s),
		UML_ADD_PAIR(4s),
		UML_ADD_PAIR(2d),
	};
#undef UML_ADD_PAIR
#define UML_ROTATIONS(name) std::array<Gadget, 4>{ \
	uml_arm64_gadget_floating_complex_##name##_0, \
	uml_arm64_gadget_floating_complex_##name##_90, \
	uml_arm64_gadget_floating_complex_##name##_180, \
	uml_arm64_gadget_floating_complex_##name##_270}
	static constexpr std::array multiply{
		UML_ROTATIONS(fcmla_4h),
		UML_ROTATIONS(fcmla_8h),
		UML_ROTATIONS(fcmla_2s),
		UML_ROTATIONS(fcmla_4s),
		UML_ROTATIONS(fcmla_2d),
	};
	static constexpr std::array half64{
		UML_ROTATIONS(fcmla_element_4h_0),
		UML_ROTATIONS(fcmla_element_4h_1),
	};
	static constexpr std::array half128{
		UML_ROTATIONS(fcmla_element_8h_0),
		UML_ROTATIONS(fcmla_element_8h_1),
		UML_ROTATIONS(fcmla_element_8h_2),
		UML_ROTATIONS(fcmla_element_8h_3),
	};
	static constexpr std::array single128{
		UML_ROTATIONS(fcmla_element_4s_0),
		UML_ROTATIONS(fcmla_element_4s_1),
	};
#undef UML_ROTATIONS
	const unsigned shape =
		test::vector_shape(form.width, form.vector_bytes);
	if (form.operation == test::Operation::add)
		return add[shape][form.rotation == 3U ? 1U : 0U];
	if (!form.by_element)
		return multiply[shape][form.rotation];
	if (form.width == 32U)
		return single128[form.index][form.rotation];
	return (form.vector_bytes == 16U ?
		half128[form.index] : half64[form.index])[form.rotation];
}

void translation_selects_every_exact_hot_path()
{
	constexpr auto base =
		test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31, 30, 29};
	std::set<std::uintptr_t> selected;
	unsigned forms{};
	test::for_each_form([&](const test::Form &form) {
		const std::array code{
			test::instruction(form, registers),
			std::uint32_t{0xd400'0001U},
		};
		const auto built = gadgets::Builder{}.build(base, code);
		require(built.complete,
			"valid complex instruction did not build");
		require(built.program.instruction_count() == 2U,
			"complex instruction changed translated block length");
		require(built.program.accesses_fp_state(),
			"complex instruction lost FP-state tracking");
		const auto words = built.program.words();
		require(words.size() >= 2U,
			"complex gadget stream is truncated");
		const auto gadget = expected_gadget(form);
		require(words[0] == address(gadget),
			"shape, rotation, or element remained runtime-decoded");
		require(words[1] == test::gadget_operand(registers),
			"register fields were not translated to direct offsets");
		selected.insert(words[0]);
		++forms;
	});
	require(forms == 62U,
		"complex form inventory changed unexpectedly");
	require(selected.size() == 62U,
		"complex gadget declarations contain a dead duplicate");
}

} // namespace

int main()
{
	translation_selects_every_exact_hot_path();
}
