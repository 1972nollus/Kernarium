// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_reciprocal_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "runtime/host_features.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>

extern "C" {
#define UML_DECLARE_RECIPROCAL_SCALAR(host, name) \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_h(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_s(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_d();
#define UML_DECLARE_RECIPROCAL_VECTOR(host, name) \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4h(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_8h(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2s(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4s(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2d();
#define UML_DECLARE_RECIPROCAL_SCALAR_PAIR(name) \
	UML_DECLARE_RECIPROCAL_SCALAR(native, name) \
	UML_DECLARE_RECIPROCAL_SCALAR(checked, name)
#define UML_DECLARE_RECIPROCAL_VECTOR_PAIR(name) \
	UML_DECLARE_RECIPROCAL_VECTOR(native, name) \
	UML_DECLARE_RECIPROCAL_VECTOR(checked, name)
UML_DECLARE_RECIPROCAL_SCALAR_PAIR(frecpe)
UML_DECLARE_RECIPROCAL_VECTOR_PAIR(frecpe)
UML_DECLARE_RECIPROCAL_SCALAR_PAIR(frecps)
UML_DECLARE_RECIPROCAL_VECTOR_PAIR(frecps)
UML_DECLARE_RECIPROCAL_SCALAR_PAIR(frecpx)
UML_DECLARE_RECIPROCAL_SCALAR_PAIR(frsqrte)
UML_DECLARE_RECIPROCAL_VECTOR_PAIR(frsqrte)
UML_DECLARE_RECIPROCAL_SCALAR_PAIR(frsqrts)
UML_DECLARE_RECIPROCAL_VECTOR_PAIR(frsqrts)
#undef UML_DECLARE_RECIPROCAL_VECTOR_PAIR
#undef UML_DECLARE_RECIPROCAL_SCALAR_PAIR
#undef UML_DECLARE_RECIPROCAL_VECTOR
#undef UML_DECLARE_RECIPROCAL_SCALAR
}

namespace test = arm64_floating_reciprocal_test;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

using Gadget = void (*)();

#define UML_RECIPROCAL_SCALAR(host, name) std::array<Gadget, 3>{ \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_h, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_s, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_d}
#define UML_RECIPROCAL_VECTOR(host, name) std::array<Gadget, 5>{ \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4h, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_8h, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2s, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4s, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2d}
#define UML_RECIPROCAL_SCALAR_PAIR(name) std::array{ \
	UML_RECIPROCAL_SCALAR(native, name), \
	UML_RECIPROCAL_SCALAR(checked, name)}
#define UML_RECIPROCAL_VECTOR_PAIR(name) std::array{ \
	UML_RECIPROCAL_VECTOR(native, name), \
	UML_RECIPROCAL_VECTOR(checked, name)}

constexpr std::array scalar_gadgets{
	UML_RECIPROCAL_SCALAR_PAIR(frecpe),
	UML_RECIPROCAL_SCALAR_PAIR(frecps),
	UML_RECIPROCAL_SCALAR_PAIR(frecpx),
	UML_RECIPROCAL_SCALAR_PAIR(frsqrte),
	UML_RECIPROCAL_SCALAR_PAIR(frsqrts),
};
constexpr std::array vector_gadgets{
	UML_RECIPROCAL_VECTOR_PAIR(frecpe),
	UML_RECIPROCAL_VECTOR_PAIR(frecps),
	std::array{std::array<Gadget, 5>{}, std::array<Gadget, 5>{}},
	UML_RECIPROCAL_VECTOR_PAIR(frsqrte),
	UML_RECIPROCAL_VECTOR_PAIR(frsqrts),
};

#undef UML_RECIPROCAL_VECTOR_PAIR
#undef UML_RECIPROCAL_SCALAR_PAIR
#undef UML_RECIPROCAL_VECTOR
#undef UML_RECIPROCAL_SCALAR

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
	const test::Form &form, test::Registers registers)
{
	return vector_offset(registers.destination) |
		(vector_offset(registers.first) << 16U) |
		(test::unary(form.operation) ? 0U :
			(vector_offset(registers.second) << 32U));
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

[[nodiscard]] unsigned conditional_branches(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(gadget);
	unsigned result = 0;
	for (unsigned index = 0; index < 64U; ++index) {
		if (indirect_branch(instructions[index]))
			return result;
		if (conditional_branch(instructions[index]))
			++result;
	}
	fail("reciprocal gadget did not reach threaded dispatch");
}

[[nodiscard]] unsigned instruction_count(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(gadget);
	for (unsigned index = 0; index < 64U; ++index)
		if (indirect_branch(instructions[index]))
			return index + 1U;
	fail("reciprocal gadget did not reach threaded dispatch");
}

[[nodiscard]] Gadget expected_gadget(const test::Form &form,
	unsigned host_variant)
{
	const unsigned operation = static_cast<unsigned>(form.operation);
	return form.scalar ?
		scalar_gadgets[operation][host_variant][
			test::scalar_shape(form.width)] :
		vector_gadgets[operation][host_variant][
			test::vector_shape(form.width, form.vector_bytes)];
}

[[nodiscard]] std::size_t valid_form_count()
{
	std::size_t count = 0;
	test::for_each_form([&](const test::Form &) {
		++count;
	});
	require(count == 35U,
		"reciprocal form formula omitted an allocated shape");
	return count;
}

void translation_selects_every_shape_and_direct_offset()
{
	constexpr test::Registers registers{31, 30, 29};
	constexpr auto base =
		test::engine_tests::engine_test_code_base;
	constexpr std::array code_tail{std::uint32_t{0xd400'0001U}};
	const unsigned host_variant =
		uml::backend::host_arm_features().afp ? 0U : 1U;
	std::set<std::uintptr_t> selected;
	std::set<std::uintptr_t> expected;
	test::for_each_form([&](const test::Form &form) {
		const std::array code{
			test::instruction(form, registers), code_tail[0]};
		const auto built = gadgets::Builder{}.build(base, code);
		require(built.complete,
			"valid reciprocal instruction did not build");
		const auto words = built.program.words();
		require(words.size() >= 2U,
			"reciprocal gadget stream is truncated");
		const auto gadget = expected_gadget(form, host_variant);
		require(words[0] == address(gadget),
			"operation or shape remained in the runtime stream");
		require(words[1] == translated_operand(form, registers),
			"register fields were not translated to direct offsets");
		selected.insert(words[0]);
		expected.insert(address(gadget));
	});
	require(expected.size() == valid_form_count(),
		"reciprocal gadget declaration table is incomplete");
	require(selected == expected,
		"reciprocal gadget table contains dead or unselected entries");
}

void every_host_variant_is_declared()
{
	std::set<std::uintptr_t> declared;
	test::for_each_form([&](const test::Form &form) {
		declared.insert(address(expected_gadget(form, 0U)));
		declared.insert(address(expected_gadget(form, 1U)));
	});
	require(declared.size() == valid_form_count() * 2U,
		"reciprocal host-variant table contains a dead duplicate");
}

void hot_paths_have_only_the_required_branches()
{
	std::set<std::uintptr_t> checked;
	unsigned maximum_native = 0U;
	unsigned maximum_checked = 0U;
	test::for_each_form([&](const test::Form &form) {
		for (unsigned host_variant = 0; host_variant < 2U;
		     ++host_variant) {
			const auto gadget =
				expected_gadget(form, host_variant);
			if (!checked.insert(address(gadget)).second)
				continue;
			require(conditional_branches(gadget) ==
					(host_variant == 0U ? 1U : 2U),
				"reciprocal hot path has an unexpected branch");
			const auto instructions = instruction_count(gadget);
			require(instructions <=
					(host_variant == 0U ? 17U : 20U),
				"reciprocal hot path exceeds its instruction budget");
			auto &maximum = host_variant == 0U ?
				maximum_native : maximum_checked;
			maximum = std::max(maximum, instructions);
		}
	});
	require(checked.size() == valid_form_count() * 2U,
		"reciprocal hot-path audit missed a gadget");
	require(maximum_native == 17U && maximum_checked == 20U,
		"reciprocal hot-path audit did not cover the largest form");
}

} // namespace

int main()
{
	translation_selects_every_shape_and_direct_offset();
	every_host_variant_is_declared();
	hot_paths_have_only_the_required_branches();
	return 0;
}
