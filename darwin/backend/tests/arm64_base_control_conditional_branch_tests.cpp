// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_control_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <set>

namespace uml::backend::tests::base_control {
namespace {

namespace gadgets = arm64::gadgets;

constexpr std::array<std::int64_t, 9> displacements{
	-1'048'576, -4096, -4, 0, 4, 8, 4096, 524'288, 1'048'572,
};

void semantics(Harness &harness)
{
	for (const bool consistent : {false, true})
		for (unsigned condition = 0; condition < 16; ++condition)
			for (unsigned flags = 0; flags < 16; ++flags)
				for (unsigned index = 0;
				     index < displacements.size(); ++index) {
					const auto displacement =
						displacements[index];
					const auto instruction =
						encode_conditional_branch(
							consistent, condition,
							displacement);
					auto initial = seeded_state(
						0x1300'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(
							instruction) << 11) ^
						flags);
					initial.nzcv = flags << 28;
					const bool take = condition_holds(
						initial.nzcv, condition);
					const auto next_pc = take ?
						static_cast<std::uint64_t>(
							static_cast<std::int64_t>(
								branch_pc) +
							displacement) :
						branch_pc + 4;
					char name[160];
					std::snprintf(name, sizeof(name),
						"%s condition=%u nzcv=%x "
						"displacement=%lld",
						consistent ? "BC.cond" :
							"B.cond",
						condition, flags,
						static_cast<long long>(
							displacement));
					harness.check_branch(
						name, instruction, initial,
						initial, next_pc,
						consistent ?
							NativeRequirements::features(
								ArmFeature::hbc) :
							NativeRequirements::baseline());
				}
}

void immediate_basis(Harness &harness)
{
	for (const bool consistent : {false, true})
		for (unsigned bit = 0; bit < 19; ++bit) {
			const auto displacement = bit == 18U ?
				-(std::int64_t{1} << 20) :
				std::int64_t{4} << bit;
			const auto instruction = encode_conditional_branch(
				consistent, 14, displacement);
			auto initial = seeded_state(
				0x1310'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 5));
			const auto target = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(branch_pc) +
				displacement);
			char name[128];
			std::snprintf(name, sizeof(name),
				"%s immediate encoding bit=%u",
				consistent ? "BC.cond" : "B.cond", bit);
			harness.check_branch(
				name, instruction, initial, initial, target,
				consistent ?
					NativeRequirements::features(ArmFeature::hbc) :
					NativeRequirements::baseline());
		}
}

void production_specialization()
{
	constexpr auto base = engine_test_code_base;
	constexpr std::int64_t displacement = 0x100;
	const auto target = base + displacement;
	std::set<std::uintptr_t> entries;
	for (unsigned condition = 0; condition < 16; ++condition) {
		const auto instruction = encode_conditional_branch(
			false, condition, displacement);
		const auto built =
			gadgets::Builder{}.build(base, std::array{instruction});
		require(built.complete && built.program.words().size() == 3U,
			"B.cond retained unexpected runtime operands");
		require(built.program.words()[1] == target &&
				built.program.words()[2] == base + 4,
			"B.cond targets were not resolved during translation");
		entries.insert(built.program.words()[0]);

		const auto consistent = gadgets::Builder{}.build(
			base, std::array{instruction | 0x10U});
		require(consistent.complete &&
				consistent.program.words().size() ==
					built.program.words().size() &&
				consistent.program.words()[0] ==
					built.program.words()[0] &&
				consistent.program.words()[1] ==
					built.program.words()[1] &&
				consistent.program.words()[2] ==
					built.program.words()[2],
			"BC.cond did not share the preselected condition gadget");
	}
	require(entries.size() == 15U,
		"B.cond did not select 14 conditions plus the always form");
}

} // namespace
} // namespace uml::backend::tests::base_control

int main()
{
	uml::backend::tests::base_control::Harness harness;
	uml::backend::tests::base_control::semantics(harness);
	uml::backend::tests::base_control::immediate_basis(harness);
	uml::backend::tests::base_control::production_specialization();
	harness.print_summary("arm64-base-control-conditional-branch");
	return 0;
}
