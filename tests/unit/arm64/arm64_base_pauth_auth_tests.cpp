// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

namespace pauth = arm64_pauth_test;
namespace test = uml::emulator::tests;

namespace {

constexpr std::array instruction_values{
	0x0000'1234'5678'9abcULL,
	0xffff'9234'5678'9abcULL,
	0x0000'0000'0000'0000ULL,
};
constexpr std::array data_values{
	0x0000'1234'5678'9abcULL,
	0xab00'1234'5678'9abcULL,
	0xffff'9234'5678'9abcULL,
};

void check_pair(
	test::EngineHarness &harness, std::string_view name,
	std::uint32_t sign, std::uint32_t authenticate,
	const pauth::arm64::CpuState &initial,
	unsigned destination, bool data)
{
	const std::array code{
		test::ProgramWord{.offset = 0, .instruction = sign},
		test::ProgramWord{.offset = 4, .instruction = authenticate},
		test::ProgramWord{.offset = 8, .instruction = 0xd400'0001U},
	};
	auto expected = initial;
	if (destination < 31U)
		expected.registers[destination] =
			pauth::strip(initial.registers[destination], data);
	expected.pc = test::engine_test_code_base + 12;
	harness.check_program({
		.name = name,
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = pauth::native_pauth,
	});
}

void check_overlapping_pair(
	test::EngineHarness &harness, std::string_view name,
	std::uint32_t sign, std::uint32_t authenticate,
	const pauth::arm64::CpuState &initial)
{
	const std::array code{
		test::ProgramWord{.offset = 0, .instruction = sign},
		test::ProgramWord{.offset = 4, .instruction = authenticate},
		test::ProgramWord{.offset = 8, .instruction = 0xd400'0001U},
	};
	auto expected = initial;
	expected.pc = test::engine_test_code_base + 12;
	harness.check_program({
		.name = name,
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = pauth::native_pauth,
		.validator = &pauth::validate_inserted_code,
	});
}

void register_modifier_forms(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (unsigned signing_operation = 0;
	     signing_operation < 4; ++signing_operation) {
		const unsigned authentication_operation =
			signing_operation + 4;
		const auto values = signing_operation >= 2U ?
			std::span<const std::uint64_t>{data_values} :
			std::span<const std::uint64_t>{instruction_values};
		for (unsigned destination = 0;
		     destination < 31; ++destination) {
			for (unsigned modifier = 0;
			     modifier < 32; ++modifier) {
				for (unsigned value_index = 0;
				     value_index < values.size(); ++value_index) {
					auto initial = pauth::initial_state(
						signing_operation * 4096 +
						destination * 128 + modifier * 4 +
						value_index);
					initial.registers[destination] =
						values[value_index];
					const pauth::CaseName name{
						"aut-%s-op%u-d%u-n%u-v%u",
						destination == modifier ?
							"overlap" : "match",
						authentication_operation, destination,
						modifier, value_index};
					const auto sign = pauth::data_processing(
						signing_operation,
						destination, modifier);
					const auto authenticate =
						pauth::data_processing(
							authentication_operation,
							destination, modifier);
					if (destination == modifier)
						check_overlapping_pair(
							harness, name, sign,
							authenticate, initial);
					else
						check_pair(
							harness, name, sign,
							authenticate, initial,
							destination,
							signing_operation >= 2U);
					++cases;
				}
			}
		}
	}
}

void same_register_failure_forms(
	test::EngineHarness &harness, std::size_t &cases)
{
	/*
	 * Xd==Xn is a valid allocation. An ordinary unsigned address is not
	 * authenticated by itself, so the architecture's failure result is
	 * implementation-defined in its inserted field. The independent oracle
	 * checks the retained address and every unaffected architectural field.
	 */
	for (unsigned operation = 4; operation < 8; ++operation) {
		const auto values = operation >= 6U ?
			std::span<const std::uint64_t>{data_values} :
			std::span<const std::uint64_t>{instruction_values};
		for (unsigned destination = 0; destination < 31; ++destination) {
			for (unsigned value_index = 0;
			     value_index < values.size(); ++value_index) {
				auto initial = pauth::initial_state(
					operation * 1024 + destination * 8 +
					value_index);
				initial.registers[destination] =
					values[value_index];
				const pauth::CaseName name{
					"aut-self-op%u-d%u-v%u",
					operation, destination, value_index};
				harness.check({
					.name = name,
					.instruction = pauth::data_processing(
						operation, destination, destination),
					.initial = initial,
					.expected = initial,
					.native = pauth::native_pauth,
					.validator =
						&pauth::validate_inserted_code,
				});
				++cases;
			}
		}
	}
}

void zero_modifier_forms(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (unsigned signing_operation = 8;
	     signing_operation < 12; ++signing_operation) {
		const unsigned authentication_operation =
			signing_operation + 4;
		const auto values = signing_operation >= 10U ?
			std::span<const std::uint64_t>{data_values} :
			std::span<const std::uint64_t>{instruction_values};
		for (unsigned destination = 0;
		     destination < 31; ++destination) {
			for (unsigned value_index = 0;
			     value_index < values.size(); ++value_index) {
				auto initial = pauth::initial_state(
					signing_operation * 1024 +
					destination * 8 + value_index);
				initial.registers[destination] =
					values[value_index];
				const pauth::CaseName name{
					"aut-zero-op%u-d%u-v%u",
					authentication_operation, destination,
					value_index};
				check_pair(
					harness, name,
					pauth::data_processing(
						signing_operation, destination),
					pauth::data_processing(
						authentication_operation,
						destination),
					initial, destination,
					signing_operation >= 10U);
				++cases;
			}
		}
	}
}

void system_alias_forms(
	test::EngineHarness &harness, std::size_t &cases)
{
	struct Pair final {
		unsigned sign;
		unsigned authenticate;
		unsigned destination;
	};
	constexpr std::array pairs{
		Pair{8, 12, 17}, Pair{10, 14, 17},
		Pair{24, 28, 30}, Pair{25, 29, 30},
		Pair{26, 30, 30}, Pair{27, 31, 30},
	};
	for (const auto pair : pairs) {
		for (unsigned value_index = 0;
		     value_index < instruction_values.size(); ++value_index) {
			auto initial = pauth::initial_state(
				pair.sign * 32 + value_index);
			initial.registers[pair.destination] =
				instruction_values[value_index];
			const pauth::CaseName name{
				"aut-alias-i%u-v%u",
				pair.authenticate, value_index};
			check_pair(
				harness, name, pauth::hint(pair.sign),
				pauth::hint(pair.authenticate), initial,
				pair.destination, false);
			++cases;
		}
	}
}

void discarded_authentication_forms(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (unsigned operation = 4; operation < 8; ++operation) {
		for (unsigned modifier = 0; modifier < 32; ++modifier) {
			const pauth::CaseName name{
				"aut-discard-op%u-n%u", operation, modifier};
			const auto initial =
				pauth::initial_state(operation * 64 + modifier);
			harness.check_one(
				name,
				pauth::data_processing(operation, 31, modifier),
				initial, initial, pauth::native_pauth);
			++cases;
		}
	}
	for (unsigned operation = 12; operation < 16; ++operation) {
		const pauth::CaseName name{
			"aut-zero-discard-op%u", operation};
		const auto initial = pauth::initial_state(operation);
		harness.check_one(
			name, pauth::data_processing(operation, 31),
			initial, initial, pauth::native_pauth);
		++cases;
	}
}

} // namespace

int main()
{
	test::EngineHarness harness;
	std::size_t cases = 0;
	register_modifier_forms(harness, cases);
	same_register_failure_forms(harness, cases);
	zero_modifier_forms(harness, cases);
	system_alias_forms(harness, cases);
	discarded_authentication_forms(harness, cases);
	pauth::require_counts(harness, cases);
	harness.print_summary("arm64-base-pauth-auth");
	return 0;
}
