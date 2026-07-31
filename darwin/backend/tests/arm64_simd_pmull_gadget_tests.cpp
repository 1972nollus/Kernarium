// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_pmull_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "runtime/host_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <set>

extern "C" {
void uml_arm64_gadget_simd_pmull_8b_direct();
void uml_arm64_gadget_simd_pmull_8b2_direct();
void uml_arm64_gadget_simd_pmull_1d_native_direct();
void uml_arm64_gadget_simd_pmull_1d2_native_direct();
void uml_arm64_gadget_simd_pmull_1d_soft_direct();
void uml_arm64_gadget_simd_pmull_1d2_soft_direct();
}

namespace test = arm64_simd_pmull_test;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

using Gadget = void (*)();

[[nodiscard]] std::uintptr_t address(Gadget gadget) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] constexpr std::uintptr_t vector_offset(
	unsigned index) noexcept
{
	return offsetof(test::arm64::CpuState, vectors) +
		index * sizeof(test::arm64::VectorRegister);
}

[[nodiscard]] constexpr std::uintptr_t operand(
	test::Registers registers) noexcept
{
	return vector_offset(registers.destination) |
		(vector_offset(registers.left) << 16) |
		(vector_offset(registers.right) << 32);
}

[[nodiscard]] Gadget expected_gadget(
	bool high, unsigned size, bool force_software)
{
	if (size == 0U)
		return high ?
			uml_arm64_gadget_simd_pmull_8b2_direct :
			uml_arm64_gadget_simd_pmull_8b_direct;
	const bool native = !force_software &&
		uml::backend::host_arm_features().has(
			uml::backend::ArmFeature::pmull);
	if (native)
		return high ?
			uml_arm64_gadget_simd_pmull_1d2_native_direct :
			uml_arm64_gadget_simd_pmull_1d_native_direct;
	return high ?
		uml_arm64_gadget_simd_pmull_1d2_soft_direct :
		uml_arm64_gadget_simd_pmull_1d_soft_direct;
}

[[nodiscard]] gadgets::BuildResult build(
	const test::Variant &variant, bool force_software)
{
	gadgets::BuildOptions options{};
	options.force_software_pmull = force_software;
	return gadgets::Builder{}.build(
		test::engine_tests::engine_test_code_base,
		std::array{test::instruction(variant)}, {},
		std::pmr::get_default_resource(), {}, options);
}

void require_translation(
	const test::Variant &variant, bool force_software,
	std::set<std::uintptr_t> &selected)
{
	const auto built = build(variant, force_software);
	const auto words = built.program.words();
	test::require(built.complete && words.size() == 4U,
		"PMULL translated stream has unexpected shape");
	const auto expected = expected_gadget(
		variant.high, variant.size, force_software);
	test::require(words[0] == address(expected),
		"PMULL implementation was not selected during translation");
	test::require(words[1] == operand(variant.registers),
		"PMULL register fields were not resolved to direct offsets");
	selected.insert(words[0]);
}

void translation_resolves_all_runtime_fields()
{
	std::set<std::uintptr_t> selected;
	for (const bool force_software : {false, true})
		for (const unsigned size : {0U, 3U})
			for (const bool high : {false, true}) {
				for (unsigned field = 0; field < 32U; ++field) {
					require_translation(
						{high, size,
						 {field, 7U, 11U}},
						force_software, selected);
					require_translation(
						{high, size,
						 {13U, field, 11U}},
						force_software, selected);
					require_translation(
						{high, size,
						 {13U, 7U, field}},
						force_software, selected);
				}
				for (const auto registers :
				     test::alias_register_cases)
					require_translation(
						{high, size, registers},
						force_software, selected);
			}
	const bool native_doubleword =
		uml::backend::host_arm_features().has(
			uml::backend::ArmFeature::pmull);
	test::require(selected.size() ==
			(native_doubleword ? 6U : 4U),
		"PMULL translation selected an unexpected gadget set");
}

[[nodiscard]] constexpr bool conditional_branch(
	std::uint32_t instruction) noexcept
{
	return (instruction & 0xff00'0010U) == 0x5400'0000U ||
		(instruction & 0x7e00'0000U) == 0x3400'0000U ||
		(instruction & 0x7e00'0000U) == 0x3600'0000U;
}

[[nodiscard]] constexpr bool direct_branch(
	std::uint32_t instruction) noexcept
{
	return (instruction & 0x7c00'0000U) == 0x1400'0000U;
}

[[nodiscard]] constexpr bool indirect_branch(
	std::uint32_t instruction) noexcept
{
	return (instruction & 0xffff'fc1fU) == 0xd61f'0000U;
}

[[nodiscard]] constexpr bool native_pmull(
	std::uint32_t instruction) noexcept
{
	return (instruction & 0xbf20'fc00U) == 0x0e20'e000U;
}

struct PathAudit final {
	unsigned instructions_before_dispatch{};
	unsigned conditional_branches{};
	unsigned direct_branches{};
	unsigned native_operations{};
};

[[nodiscard]] PathAudit audit(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(gadget);
	PathAudit result{};
	for (unsigned index = 0; index < 32U; ++index) {
		const auto current = instructions[index];
		if (indirect_branch(current))
			return result;
		++result.instructions_before_dispatch;
		result.conditional_branches += conditional_branch(current);
		result.direct_branches += direct_branch(current);
		result.native_operations += native_pmull(current);
	}
	test::fail("PMULL gadget did not reach threaded dispatch");
}

void exact_definition_and_hot_path_budgets()
{
	constexpr std::array<Gadget, 4> native{
		uml_arm64_gadget_simd_pmull_8b_direct,
		uml_arm64_gadget_simd_pmull_8b2_direct,
		uml_arm64_gadget_simd_pmull_1d_native_direct,
		uml_arm64_gadget_simd_pmull_1d2_native_direct,
	};
	constexpr std::array<Gadget, 2> software{
		uml_arm64_gadget_simd_pmull_1d_soft_direct,
		uml_arm64_gadget_simd_pmull_1d2_soft_direct,
	};
	std::set<std::uintptr_t> definitions;
	for (const auto gadget : native) {
		definitions.insert(address(gadget));
		const auto path = audit(gadget);
		test::require(path.instructions_before_dispatch == 9U,
			"native PMULL hot path instruction budget changed");
		test::require(path.conditional_branches == 0U &&
				path.direct_branches == 0U,
			"native PMULL hot path contains a runtime branch");
		test::require(path.native_operations == 1U,
			"native PMULL gadget does not contain exactly one operation");
	}
	for (const auto gadget : software) {
		definitions.insert(address(gadget));
		const auto path = audit(gadget);
		test::require(path.instructions_before_dispatch == 8U,
			"portable PMULL path instruction budget changed");
		test::require(path.conditional_branches == 0U &&
				path.direct_branches == 1U,
			"portable PMULL path has unexpected control flow");
		test::require(path.native_operations == 0U,
			"portable PMULL gadget contains the optional operation");
	}
	test::require(definitions.size() == 6U,
		"PMULL gadget definition set has a duplicate or missing entry");
}

} // namespace

int main()
{
	translation_resolves_all_runtime_fields();
	exact_definition_and_hot_path_budgets();
	return 0;
}
