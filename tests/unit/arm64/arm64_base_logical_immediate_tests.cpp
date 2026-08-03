// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_logical_test_support.hpp"

#include <cstdio>

namespace uml::emulator::tests::base_logical {
namespace {

[[nodiscard]] constexpr std::string_view operation_name(
	unsigned operation) noexcept
{
	switch (operation) {
	case 0:
		return "AND";
	case 1:
		return "ORR";
	case 2:
		return "EOR";
	default:
		return "ANDS";
	}
}

void every_encoding_field(EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 4; ++operation)
			for (unsigned n = 0; n < 2; ++n)
				for (unsigned rotate = 0; rotate < 64; ++rotate)
					for (unsigned set_bits = 0; set_bits < 64;
					     ++set_bits) {
						const unsigned source =
							(rotate + set_bits * 3U) & 31U;
						const unsigned destination =
							(rotate * 5U + set_bits +
							 operation * 7U) & 31U;
						const auto instruction = encode_immediate(
							wide, operation, n, rotate, set_bits,
							source, destination);
						auto initial = seeded_state(
							0x1000'0000'0000'0000ULL ^
							(static_cast<std::uint64_t>(
								instruction) << 17) ^
							(static_cast<std::uint64_t>(
								rotate) << 48) ^
							set_bits);
						char name[152];
						const auto operation_text =
							operation_name(operation);
						std::snprintf(name, sizeof(name),
							"%.*s immediate width=%u N=%u "
							"immr=%u imms=%u rn=%u rd=%u",
							static_cast<int>(
								operation_text.size()),
							operation_text.data(),
							wide ? 64U : 32U, n, rotate,
							set_bits, source, destination);
						if (is_valid(Form::immediate,
							    instruction))
							check(harness, name,
								Form::immediate,
								instruction, initial);
						else
							check_illegal(harness, name,
								Form::immediate,
								instruction, initial);
					}
}

void every_register_pair(EngineHarness &harness)
{
	/* This valid 32-bit element encoding is allocated for both widths. */
	constexpr unsigned n = 0;
	constexpr unsigned rotate = 11;
	constexpr unsigned set_bits = 5;
	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 4; ++operation)
			for (unsigned source = 0; source < 32; ++source)
				for (unsigned destination = 0; destination < 32;
				     ++destination) {
					const auto instruction = encode_immediate(
						wide, operation, n, rotate, set_bits,
						source, destination);
					auto initial = seeded_state(
						0x1100'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(
							source) << 45) ^
						(static_cast<std::uint64_t>(
							destination) << 37) ^
						instruction);
					char name[128];
					const auto operation_text =
						operation_name(operation);
					std::snprintf(name, sizeof(name),
						"%.*s immediate register matrix "
						"width=%u rn=%u rd=%u",
						static_cast<int>(
							operation_text.size()),
						operation_text.data(),
						wide ? 64U : 32U, source,
						destination);
					check(harness, name, Form::immediate,
						instruction, initial);
				}
}

} // namespace

void run_immediate_tests(EngineHarness &harness)
{
	every_encoding_field(harness);
	every_register_pair(harness);
}

} // namespace uml::emulator::tests::base_logical
