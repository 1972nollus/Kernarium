// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_aes_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "runtime/host_features.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <set>

extern "C" {
void uml_arm64_gadget_simd_aese_native();
void uml_arm64_gadget_simd_aese_soft();
void uml_arm64_gadget_simd_aesd_native();
void uml_arm64_gadget_simd_aesd_soft();
void uml_arm64_gadget_simd_aesmc_native();
void uml_arm64_gadget_simd_aesmc_soft();
void uml_arm64_gadget_simd_aesimc_native();
void uml_arm64_gadget_simd_aesimc_soft();
}

namespace test = arm64_simd_aes_test;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

using Gadget = void (*)();

constexpr std::array operations{
	test::Operation::encrypt_round,
	test::Operation::decrypt_round,
	test::Operation::mix_columns,
	test::Operation::inverse_mix_columns,
};

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

[[nodiscard]] Gadget expected_gadget(
	test::Operation operation, bool native)
{
	switch (operation) {
	case test::Operation::encrypt_round:
		return native ? uml_arm64_gadget_simd_aese_native :
			uml_arm64_gadget_simd_aese_soft;
	case test::Operation::decrypt_round:
		return native ? uml_arm64_gadget_simd_aesd_native :
			uml_arm64_gadget_simd_aesd_soft;
	case test::Operation::mix_columns:
		return native ? uml_arm64_gadget_simd_aesmc_native :
			uml_arm64_gadget_simd_aesmc_soft;
	case test::Operation::inverse_mix_columns:
		return native ? uml_arm64_gadget_simd_aesimc_native :
			uml_arm64_gadget_simd_aesimc_soft;
	}
	return nullptr;
}

[[nodiscard]] constexpr bool conditional_branch(
	std::uint32_t instruction) noexcept
{
	return (instruction & 0xff00'0010U) == 0x5400'0000U ||
		(instruction & 0x7e00'0000U) == 0x3400'0000U ||
		(instruction & 0x7e00'0000U) == 0x3600'0000U;
}

[[nodiscard]] constexpr bool indirect_branch(
	std::uint32_t instruction) noexcept
{
	return (instruction & 0xffff'fc1fU) == 0xd61f'0000U;
}

[[nodiscard]] unsigned instruction_count(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(gadget);
	for (unsigned index = 0; index < 32U; ++index)
		if (indirect_branch(instructions[index]))
			return index + 1U;
	test::fail("block-transform gadget did not reach threaded dispatch");
}

[[nodiscard]] unsigned conditional_branches(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(gadget);
	unsigned branches = 0;
	for (unsigned index = 0; index < 32U; ++index) {
		if (indirect_branch(instructions[index]))
			return branches;
		if (conditional_branch(instructions[index]))
			++branches;
	}
	test::fail("block-transform gadget did not reach threaded dispatch");
}

void translation_specializes_operation_host_path_and_registers()
{
	const bool native =
		uml::backend::host_arm_features().has(
			uml::backend::ArmFeature::advanced_simd) &&
		uml::backend::host_arm_features().has(
			uml::backend::ArmFeature::aes);
	constexpr test::Registers registers{31, 30};
	std::set<std::uintptr_t> selected;
	for (const auto operation : operations) {
		const test::Variant variant{operation, registers};
		const auto built = gadgets::Builder{}.build(
			test::engine_tests::engine_test_code_base,
			std::array{test::instruction(variant)});
		const auto words = built.program.words();
		test::require(built.complete && words.size() > 2U,
			"block-transform gadget stream is truncated");
		test::require(words[0] ==
				address(expected_gadget(operation, native)),
			"operation or host path remained in the runtime stream");
		test::require(
			words[1] == vector_offset(registers.destination) &&
				words[2] == vector_offset(registers.source),
			"register fields were not translated to direct offsets");
		selected.insert(words[0]);

		gadgets::BuildOptions options{};
		options.force_software_aes = true;
		const auto portable = gadgets::Builder{}.build(
			test::engine_tests::engine_test_code_base,
			std::array{test::instruction(variant)}, {},
			std::pmr::get_default_resource(), {}, options);
		const auto portable_words = portable.program.words();
		test::require(portable.complete &&
				portable_words.size() > 2U,
			"portable block-transform stream is truncated");
		test::require(portable_words[0] ==
				address(expected_gadget(operation, false)),
			"portable operation remained selected at runtime");
		test::require(
			portable_words[1] ==
				vector_offset(registers.destination) &&
				portable_words[2] ==
				vector_offset(registers.source),
			"portable registers retained runtime decode");
	}
	test::require(selected.size() == operations.size(),
		"translation did not select every current-host gadget");
}

void every_definition_is_reachable_and_branch_free()
{
	std::set<std::uintptr_t> definitions;
	unsigned maximum_native = 0;
	unsigned maximum_software = 0;
	for (const auto operation : operations)
		for (const bool native : {false, true}) {
			const auto gadget = expected_gadget(operation, native);
			definitions.insert(address(gadget));
			test::require(conditional_branches(gadget) == 0U,
				"valid gadget contains a conditional branch");
			auto &maximum =
				native ? maximum_native : maximum_software;
			maximum = std::max(maximum,
				instruction_count(gadget));
		}
	test::require(definitions.size() == 8U,
		"block-transform gadget table has a duplicate entry");
	test::require(maximum_native <= 7U,
		"native block-transform hot path exceeds its instruction budget");
	test::require(maximum_software <= 9U,
		"portable block-transform path exceeds its instruction budget");
}

} // namespace

int main()
{
	translation_specializes_operation_host_path_and_registers();
	every_definition_is_reachable_and_branch_free();
	return 0;
}
