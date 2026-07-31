// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_control_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>

namespace uml::backend::tests::base_control {
namespace {

namespace gadgets = arm64::gadgets;

constexpr std::array<std::int64_t, 17> displacements{
	-(std::int64_t{1} << 27),
	-(std::int64_t{1} << 26),
	-4096,
	-16,
	-8,
	-4,
	0,
	4,
	8,
	12,
	16,
	4092,
	4096,
	(std::int64_t{1} << 12),
	(std::int64_t{1} << 20),
	(std::int64_t{1} << 26),
	(std::int64_t{1} << 27) - 4,
};

void semantics(Harness &harness)
{
	for (const bool link : {false, true})
		for (unsigned index = 0; index < displacements.size(); ++index) {
			const auto displacement = displacements[index];
			const auto instruction =
				encode_direct_branch(link, displacement);
			auto initial = seeded_state(
				0x1200'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 9) ^
				index);
			initial.registers[30] = 0xfeed'face'cafe'beefULL;
			auto expected = initial;
			if (link)
				expected.registers[30] =
					branch_pc + sizeof(std::uint32_t);
			const auto target = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(branch_pc) +
				displacement);
			char name[128];
			std::snprintf(name, sizeof(name), "%s displacement=%lld",
				link ? "BL" : "B",
				static_cast<long long>(displacement));
			harness.check_branch(name, instruction, initial, expected,
				target);
		}
}

void immediate_basis(Harness &harness)
{
	for (const bool link : {false, true})
		for (unsigned bit = 0; bit < 26; ++bit) {
			const auto displacement = bit == 25U ?
				-(std::int64_t{1} << 27) :
				std::int64_t{4} << bit;
			const auto instruction =
				encode_direct_branch(link, displacement);
			auto initial = seeded_state(
				0x1210'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 5));
			auto expected = initial;
			if (link)
				expected.registers[30] = branch_pc + 4;
			const auto target = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(branch_pc) +
				displacement);
			char name[112];
			std::snprintf(name, sizeof(name),
				"%s immediate encoding bit=%u",
				link ? "BL" : "B", bit);
			harness.check_branch(
				name, instruction, initial, expected, target);
		}
}

void production_specialization()
{
	constexpr auto base = engine_test_code_base;
	constexpr std::int64_t displacement = 0x1234;
	const auto target = base + displacement;
	const auto branch = gadgets::Builder{}.build(
		base, std::array{encode_direct_branch(false, displacement)});
	const auto link = gadgets::Builder{}.build(
		base, std::array{encode_direct_branch(true, displacement)});
	require(branch.complete && link.complete,
		"direct branch production program did not build");
	require(branch.program.words().size() == 2U &&
			branch.program.words()[1] == target,
		"B target was not resolved during translation");
	require(link.program.words().size() == 3U &&
			link.program.words()[1] == target &&
			link.program.words()[2] == base + 4,
		"BL target/link values were not resolved during translation");
	require(branch.program.words()[0] != link.program.words()[0],
		"B and BL did not select distinct straight-line gadgets");
}

} // namespace
} // namespace uml::backend::tests::base_control

int main()
{
	uml::backend::tests::base_control::Harness harness;
	uml::backend::tests::base_control::semantics(harness);
	uml::backend::tests::base_control::immediate_basis(harness);
	uml::backend::tests::base_control::production_specialization();
	harness.print_summary("arm64-base-control-direct-branch");
	return 0;
}
