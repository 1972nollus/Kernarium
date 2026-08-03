// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_mops_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

namespace mops = mops_test;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace engine = uml::emulator::tests;

namespace {

constexpr unsigned destination_register = 3;
constexpr unsigned source_register = 4;
constexpr unsigned count_register = 5;

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	return gadgets::Builder{}.build(
		engine::engine_test_code_base, std::array{instruction});
}

[[nodiscard]] constexpr std::uintptr_t register_offset(
	unsigned index) noexcept
{
	return index == 31U ?
		offsetof(mops::arm64::CpuState, gadget_zero) :
		index * sizeof(std::uint64_t);
}

[[nodiscard]] constexpr std::uintptr_t operand(unsigned source) noexcept
{
	return register_offset(destination_register) |
		(register_offset(count_register) << 16) |
		(register_offset(source) << 32);
}

[[nodiscard]] bool same_words(const gadgets::BuildResult &left,
	const gadgets::BuildResult &right)
{
	return left.program.words().size() == right.program.words().size() &&
		std::equal(left.program.words().begin(), left.program.words().end(),
			right.program.words().begin());
}

void require_operand(const gadgets::BuildResult &built,
	std::uintptr_t expected, bool carries_pc)
{
	mops::require(built.complete, "MOPS gadget translation completeness");
	mops::require(built.program.instruction_count() == 1,
		"MOPS gadget instruction count");
	mops::require(built.program.words().size() >= (carries_pc ? 3U : 2U),
		"MOPS gadget word count");
	mops::require(built.program.words()[1] == expected,
		"MOPS direct CpuState operand");
	if (carries_pc)
		mops::require(built.program.words()[2] ==
			engine::engine_test_code_base,
			"MOPS fault continuation PC");
}

void copy_option_aliases(std::set<std::uintptr_t> &entries)
{
	for (const bool general : {false, true}) {
		for (const auto stage :
		     {mops::Stage::prologue, mops::Stage::main,
		      mops::Stage::epilogue}) {
			const auto reference = build(mops::copy_instruction(general,
				stage, 0, destination_register, source_register,
				count_register));
			mops::require(reference.complete,
				"copy gadget translation completeness");
			entries.insert(reference.program.words().front());
			for (unsigned options = 1; options < 16; ++options) {
				const auto alias = build(mops::copy_instruction(general,
					stage, options, destination_register,
					source_register, count_register));
				mops::require(alias.complete,
					"copy option alias completeness");
				mops::require(same_words(alias, reference),
					"copy option eliminated at translation");
			}
			if (stage != mops::Stage::main)
				require_operand(reference, operand(source_register),
					stage == mops::Stage::epilogue);
		}
	}
}

void set_option_aliases(std::set<std::uintptr_t> &entries, bool tags)
{
	for (const auto stage :
	     {mops::Stage::prologue, mops::Stage::main,
	      mops::Stage::epilogue}) {
		for (const unsigned source : {source_register, 31U}) {
			const auto instruction = tags ?
				mops::set_tag_instruction(stage, 0,
					destination_register, count_register, source) :
				mops::set_instruction(stage, 0,
					destination_register, count_register, source);
			const auto reference = build(instruction);
			mops::require(reference.complete,
				"set gadget translation completeness");
			entries.insert(reference.program.words().front());
			for (unsigned options = 1; options < 4; ++options) {
				const auto alias = build(tags ?
					mops::set_tag_instruction(stage, options,
						destination_register, count_register, source) :
					mops::set_instruction(stage, options,
						destination_register, count_register, source));
				mops::require(alias.complete,
					"set option alias completeness");
				mops::require(same_words(alias, reference),
					"set option eliminated at translation");
			}
			if (stage != mops::Stage::main)
				require_operand(reference, operand(source),
					tags || stage == mops::Stage::epilogue);
		}
	}
}

void translation_specialization()
{
	std::set<std::uintptr_t> entries;
	copy_option_aliases(entries);
	set_option_aliases(entries, false);
	set_option_aliases(entries, true);
	/*
	 * One common no-op main, three prologues, four ordinary epilogues,
	 * and three tag-setting entries. No runtime entry decodes option,
	 * family, stage, or XZR choices.
	 */
	mops::require(entries.size() == 11,
		"MOPS specialized gadget entry count");
}

} // namespace

int main()
{
	translation_specialization();
	std::puts("arm64-base-mops-gadget: specialization passed");
}
