// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcsel_test_support.hpp"

#include <array>

namespace test = arm64_simd_fcsel_test;

namespace {

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

constexpr std::array register_aliases{
	Registers{28U, 29U, 30U},
	Registers{29U, 29U, 30U},
	Registers{30U, 29U, 30U},
	Registers{28U, 29U, 29U},
	Registers{29U, 29U, 29U},
};

void every_precision_condition_flags_alias_and_control_mode()
{
	test::engine_tests::EngineHarness harness;
	for (const unsigned bytes : {2U, 4U, 8U})
		for (unsigned condition = 0; condition < 16U; ++condition)
			for (unsigned flags = 0; flags < 16U; ++flags)
				for (const auto registers : register_aliases)
					for (unsigned control_mode = 0;
					     control_mode < 4U; ++control_mode) {
						auto initial = test::state(bytes * 31U +
							condition * 17U + flags * 7U +
							control_mode * 43U +
							registers.destination);
						initial.nzcv = flags << 28;
						initial.fpcr = 0x0008'0000U |
							(control_mode << 22);
						/*
						 * Preserve unusual payloads verbatim. FCSEL does not
						 * classify, round, quiet, or otherwise interpret them.
						 */
						initial.vectors[29].bytes = {
							std::byte{0x01}, std::byte{0x7e},
							std::byte{0x01}, std::byte{0x00},
							std::byte{0x00}, std::byte{0x00},
							std::byte{0xf8}, std::byte{0x7f},
							std::byte{0xa5}, std::byte{0x5a},
							std::byte{0xc3}, std::byte{0x3c},
							std::byte{0x96}, std::byte{0x69},
							std::byte{0x87}, std::byte{0x78},
						};
						initial.vectors[30].bytes = {
							std::byte{0x00}, std::byte{0x80},
							std::byte{0x00}, std::byte{0x00},
							std::byte{0x80}, std::byte{0xff},
							std::byte{0xff}, std::byte{0xff},
							std::byte{0x5a}, std::byte{0xa5},
							std::byte{0x3c}, std::byte{0xc3},
							std::byte{0x69}, std::byte{0x96},
							std::byte{0x78}, std::byte{0x87},
						};
						harness.check_one("FCSEL behavior",
							test::instruction(bytes, condition,
								registers.destination,
								registers.first, registers.second),
							initial,
							test::expected_state(initial, bytes,
								condition, registers.destination,
								registers.first, registers.second),
							test::native_requirements(bytes));
					}
	harness.print_summary("simd-fcsel-behavior");
}

} // namespace

int main()
{
	every_precision_condition_flags_alias_and_control_mode();
	return 0;
}
