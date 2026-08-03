// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_min_max_test_support.hpp"

#include <cstdio>

namespace test = uml::emulator::tests::base_min_max;
namespace engine_tests = uml::emulator::tests;

namespace {

void encoding_examples()
{
	test::require(test::encode_immediate(false,
			test::Operation::signed_maximum, 0x80, 12, 13) ==
			0x11c2'01acU,
		"SMAX immediate W encoding mismatch");
	test::require(test::encode_immediate(true,
			test::Operation::signed_minimum, 0xff, 18, 19) ==
			0x91cb'fe72U,
		"SMIN immediate X encoding mismatch");
	test::require(test::encode_immediate(false,
			test::Operation::unsigned_maximum, 0xff, 20, 21) ==
			0x11c7'feb4U,
		"UMAX immediate W encoding mismatch");
	test::require(test::encode_immediate(true,
			test::Operation::unsigned_minimum, 0x80, 26, 27) ==
			0x91ce'037aU,
		"UMIN immediate X encoding mismatch");
	test::require(test::encode_register(false,
			test::Operation::signed_maximum, 0, 1, 2) ==
			0x1ac2'6020U,
		"SMAX register W encoding mismatch");
	test::require(test::encode_register(true,
			test::Operation::signed_minimum, 9, 10, 11) ==
			0x9acb'6949U,
		"SMIN register X encoding mismatch");
	test::require(test::encode_register(false,
			test::Operation::unsigned_maximum, 12, 13, 14) ==
			0x1ace'65acU,
		"UMAX register W encoding mismatch");
	test::require(test::encode_register(true,
			test::Operation::unsigned_minimum, 21, 22, 23) ==
			0x9ad7'6ed5U,
		"UMIN register X encoding mismatch");
}

void reserved_s_partition(engine_tests::EngineHarness &harness,
	std::size_t &cases)
{
	/*
	 * The four S==0 opcodes 24..27 are the allocated min/max family.
	 * Their complete S==1 partition is unallocated.  Vary each register
	 * field independently, including ZR and the discarded destination.
	 */
	for (const bool wide : {false, true})
		for (const auto description : test::descriptions) {
			for (unsigned rd = 0; rd < 32U; ++rd) {
				auto initial = test::state(
					0x1000'0000'0000'0000ULL ^ rd);
				char name[192];
				std::snprintf(name, sizeof(name),
					"%.*s reserved-S destination width=%u rd=%u",
					static_cast<int>(
						description.register_id.size()),
					description.register_id.data(),
					wide ? 64U : 32U, rd);
				harness.check_illegal(name,
					test::encode_register(wide,
						description.operation, rd, 3,
						5) |
						0x2000'0000U,
					initial, test::native());
				++cases;
			}
			for (unsigned rn = 0; rn < 32U; ++rn) {
				auto initial = test::state(
					0x2000'0000'0000'0000ULL ^ rn);
				char name[192];
				std::snprintf(name, sizeof(name),
					"%.*s reserved-S first-source width=%u rn=%u",
					static_cast<int>(
						description.register_id.size()),
					description.register_id.data(),
					wide ? 64U : 32U, rn);
				harness.check_illegal(name,
					test::encode_register(wide,
						description.operation, 7, rn,
						5) |
						0x2000'0000U,
					initial, test::native());
				++cases;
			}
			for (unsigned rm = 0; rm < 32U; ++rm) {
				auto initial = test::state(
					0x3000'0000'0000'0000ULL ^ rm);
				char name[192];
				std::snprintf(name, sizeof(name),
					"%.*s reserved-S second-source width=%u rm=%u",
					static_cast<int>(
						description.register_id.size()),
					description.register_id.data(),
					wide ? 64U : 32U, rm);
				harness.check_illegal(name,
					test::encode_register(wide,
						description.operation, 7, 3,
						rm) |
						0x2000'0000U,
					initial, test::native());
				++cases;
			}
		}
}

} // namespace

int main()
{
	encoding_examples();
	engine_tests::EngineHarness harness;
	std::size_t cases{};
	reserved_s_partition(harness, cases);
	const auto &counts = harness.counts();
	test::require(counts.interpreter == cases &&
			counts.gadgets == cases && counts.jit == cases,
		"reserved min/max partition was not rejected by every engine");
	test::require(counts.jit_skipped == 0 &&
			counts.jit_unavailable == 0,
		"invalid min/max encodings were incorrectly feature-skipped");
	harness.print_summary("arm64-base-min-max-allocation");
	return 0;
}
