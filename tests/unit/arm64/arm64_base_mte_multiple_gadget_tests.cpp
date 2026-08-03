// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_mte_multiple_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <utility>

namespace test = arm64_base_mte_multiple_test;
namespace feature_test = arm64_base_feature_system_test;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace engine = uml::emulator::tests;

namespace {

void all_forms_fold_to_one_illegal_terminal()
{
	std::uintptr_t illegal_entry = 0;
	for (const auto base : {
	     test::ldgm_base, test::stgm_base, test::stzgm_base}) {
		for (const auto fields : {
		     std::pair{0U, 0U},
		     std::pair{0U, 31U},
		     std::pair{31U, 0U},
		     std::pair{31U, 31U},
		     std::pair{7U, 19U},
		     }) {
			const auto built = gadgets::Builder{}.build(
				engine::engine_test_code_base,
				std::array{test::instruction(
					base, fields.first, fields.second)});
			feature_test::require(built.complete);
			feature_test::require(
				built.program.instruction_count() == 1U);
			feature_test::require(built.program.words().size() == 2U);
			if (illegal_entry == 0)
				illegal_entry = built.program.words().front();
			feature_test::require(
				built.program.words().front() == illegal_entry);
		}
	}
	feature_test::require(illegal_entry != 0);
}

} // namespace

int main()
{
	all_forms_fold_to_one_illegal_terminal();
	std::puts("arm64-base-mte-multiple-gadget: specialization passed");
}
