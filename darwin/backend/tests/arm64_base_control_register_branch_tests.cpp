// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_control_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

namespace uml::backend::tests::base_control {
namespace {

namespace gadgets = arm64::gadgets;

constexpr std::array operations{
	RegisterBranch::branch,
	RegisterBranch::branch_link,
	RegisterBranch::return_,
};

[[nodiscard]] constexpr const char *name(RegisterBranch operation) noexcept
{
	return operation == RegisterBranch::branch ? "BR" :
		operation == RegisterBranch::branch_link ? "BLR" : "RET";
}

void semantics(Harness &harness)
{
	for (const auto operation : operations)
		for (unsigned source = 0; source < 32; ++source) {
			const auto instruction =
				encode_register_branch(operation, source);
			auto initial = seeded_state(
				0x1600'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 17));
			initial.sp = 0xdead'beef'dead'beefULL;
			if (source != 31U)
				initial.registers[source] =
					0x7000'0000ULL +
					static_cast<std::uint64_t>(source) *
						DARWIN_UML_GUEST_PAGE_SIZE;
			const auto target = source == 31U ? 0U :
				initial.registers[source];
			auto expected = initial;
			if (operation == RegisterBranch::branch_link)
				expected.registers[30] = branch_pc + 4;
			char case_name[96];
			std::snprintf(case_name, sizeof(case_name), "%s x%u",
				name(operation), source);
			harness.check_register_branch(
				case_name, instruction, initial, expected, target);
		}
}

void production_specialization()
{
	constexpr auto base = engine_test_code_base;
	std::set<std::uintptr_t> entries;
	for (const auto operation : operations)
		for (const unsigned source : {0U, 7U, 30U, 31U}) {
			const auto built = gadgets::Builder{}.build(
				base, std::array{
					encode_register_branch(operation, source)});
			require(built.complete,
				"register branch production program did not build");
			const auto expected_size =
				operation == RegisterBranch::branch_link ? 3U : 2U;
			require(built.program.words().size() == expected_size,
				"register branch retained unexpected runtime fields");
			const auto offset = source == 31U ?
				offsetof(arm64::CpuState, gadget_zero) :
				offsetof(arm64::CpuState, registers) +
					source * 8U;
			require(built.program.words()[1] == offset,
				"register branch source was not resolved during "
				"translation");
			if (operation == RegisterBranch::branch_link)
				require(built.program.words()[2] == base + 4,
					"BLR link value was not resolved during "
					"translation");
			entries.insert(built.program.words()[0]);
		}
	require(entries.size() == 3U,
		"BR, BLR, and RET did not select distinct gadgets");
}

} // namespace
} // namespace uml::backend::tests::base_control

int main()
{
	uml::backend::tests::base_control::Harness harness;
	uml::backend::tests::base_control::semantics(harness);
	uml::backend::tests::base_control::production_specialization();
	harness.print_summary("arm64-base-control-register-branch");
	return 0;
}
