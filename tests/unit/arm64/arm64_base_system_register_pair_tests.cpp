// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_system_operation_test_support.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdlib>

namespace operations = arm64_base_system_operation_test;
namespace engine = uml::emulator::tests;
namespace feature_test = arm64_base_feature_system_test;
using ::uml::cpu::ArmFeature;

namespace {

[[nodiscard]] unsigned argument(const char *text, unsigned limit)
{
	char *end = nullptr;
	errno = 0;
	const auto value = std::strtoul(text, &end, 10);
	feature_test::require(
		errno == 0 && end != text && *end == '\0' && value < limit);
	return static_cast<unsigned>(value);
}

void encoding_examples()
{
	feature_test::require(
		operations::system_register_pair(
			false, 0, 0, 0, 0, 0, 0) == 0xd550'0000U);
	feature_test::require(
		operations::system_register_pair(
			true, 1, 7, 15, 15, 7, 30) == 0xd57f'fffeU);
}

void selector_shard(engine::EngineHarness &harness, bool read,
	unsigned selected_op1)
{
	const auto native = engine::NativeRequirements::features(
		ArmFeature::sysreg128);
	for (unsigned o0 = 0; o0 < 2; ++o0)
		for (unsigned crn = 0; crn < 16; ++crn)
			for (unsigned crm = 0; crm < 16; ++crm)
				for (unsigned op2 = 0; op2 < 8; ++op2)
					for (unsigned first = 0; first < 32;
					     ++first) {
						const auto instruction =
							operations::
								system_register_pair(
									read, o0,
									selected_op1,
									crn, crm,
									op2, first);
						const feature_test::CaseName name{
							"%s-o0%u-op1%u-c%u-c%u-op2%u-t%u",
							read ? "mrrs" : "msrr",
							o0, selected_op1, crn,
							crm, op2, first};
						if ((first & 1U) == 0U)
							feature_test::
								check_profile_illegal(
									harness,
									name,
									instruction,
									native);
						else
							harness.check_illegal(
								name,
								instruction,
								arm64_system_test::
									state());
					}
}

} // namespace

int main(int argc, char **argv)
{
	feature_test::require(argc == 3);
	const bool read = argument(argv[1], 2) != 0;
	const unsigned op1 = argument(argv[2], 8);
	encoding_examples();
	engine::EngineHarness harness;
	selector_shard(harness, read, op1);
	constexpr std::size_t cases = 2U * 16U * 16U * 8U * 32U;
	/* Every odd Rt is reserved and must execute JIT even without the
	 * extension; the even half is dynamically feature-gated. */
	operations::require_mandatory_jit(harness, cases, cases / 2U);
	harness.print_summary(read ?
		"arm64-base-mrrs-allocation" :
		"arm64-base-msrr-allocation");
}
