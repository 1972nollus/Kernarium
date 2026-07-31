// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_gic_test_support.hpp"

#include <array>
#include <cstddef>

namespace gic = arm64_base_gic_test;
namespace engine = uml::backend::tests;
namespace sys_test = arm64_system_test;

namespace {

struct Operation final {
	unsigned op1;
	unsigned crm;
	unsigned op2;
};

constexpr std::array operations{
	Operation{0, 1, 0}, Operation{0, 1, 1},
	Operation{0, 1, 2}, Operation{0, 1, 3},
	Operation{0, 1, 4}, Operation{0, 1, 5},
	Operation{0, 1, 7}, Operation{0, 2, 0},
	Operation{0, 2, 1},
	Operation{4, 1, 0}, Operation{4, 1, 1},
	Operation{4, 1, 2}, Operation{4, 1, 3},
	Operation{4, 1, 4}, Operation{4, 1, 5},
	Operation{4, 2, 0}, Operation{4, 2, 1},
	Operation{6, 1, 0}, Operation{6, 1, 1},
	Operation{6, 1, 2}, Operation{6, 1, 3},
	Operation{6, 1, 4}, Operation{6, 1, 5},
	Operation{6, 2, 0}, Operation{6, 2, 1},
};

static_assert(operations.size() == 25);

void encoding_examples()
{
	gic::require(sys_test::system_instruction(false, 0, 12, 1, 0, 31) ==
		0xd508'c11fU);
	gic::require(sys_test::system_instruction(false, 4, 12, 2, 1, 7) ==
		0xd50c'c227U);
	gic::require(sys_test::system_instruction(false, 6, 12, 1, 5, 0) ==
		0xd50e'c1a0U);
}

void all_named_operations_and_transfers(engine::EngineHarness &harness)
{
	for (const auto operation : operations)
		for (unsigned transfer = 0; transfer < 32; ++transfer) {
			const auto instruction = sys_test::system_instruction(false,
				operation.op1, 12, operation.crm, operation.op2,
				transfer);
			const gic::CaseName name{"gic-op1%u-crm%u-op2%u-t%u",
				operation.op1, operation.crm, operation.op2,
				transfer};
			gic::check_feature_disabled(harness, name, instruction);
		}
}

} // namespace

int main()
{
	encoding_examples();
	engine::EngineHarness harness;
	all_named_operations_and_transfers(harness);
	constexpr std::size_t cases = operations.size() * 32;
	gic::require_counts(harness, cases);
	harness.print_summary("arm64-base-gic");
}
