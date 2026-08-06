// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

#include <set>

namespace pauth = arm64_pauth_test;
namespace test = uml::emulator::tests;

namespace {

constexpr std::array values{
	0x0000'1234'5678'9abcULL,
	0xab00'1234'5678'9abcULL,
	0xffff'9234'5678'9abcULL,
	0x0000'0000'0000'0000ULL,
};

void register_modifier_forms(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (unsigned operation = 0; operation < 4; ++operation) {
		for (unsigned destination = 0; destination < 32; ++destination) {
			for (unsigned modifier = 0; modifier < 32; ++modifier) {
				for (unsigned value_index = 0;
				     value_index < values.size(); ++value_index) {
					auto initial = pauth::initial_state(
						operation * 4096 + destination * 128 +
						modifier * 4 + value_index);
					if (destination < 31U)
						initial.registers[destination] =
							values[value_index];
					const pauth::CaseName name{
						"pac-register-op%u-d%u-n%u-v%u",
						operation, destination, modifier,
						value_index};
					harness.check({
						.name = name,
						.instruction = pauth::data_processing(
							operation, destination, modifier),
						.initial = initial,
						.expected = initial,
						.native = pauth::native_pauth,
						.validator =
							&pauth::validate_inserted_code,
						.validator_relation_id =
							"permitted-transformed-code/v1",
					});
					++cases;
				}
			}
		}
	}
}

void zero_modifier_forms(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (unsigned operation = 8; operation < 12; ++operation) {
		for (unsigned destination = 0; destination < 32; ++destination) {
			for (unsigned value_index = 0;
			     value_index < values.size(); ++value_index) {
				auto initial = pauth::initial_state(
					operation * 1024 + destination * 8 + value_index);
				if (destination < 31U)
					initial.registers[destination] =
						values[value_index];
				const pauth::CaseName name{
					"pac-zero-op%u-d%u-v%u",
					operation, destination, value_index};
				harness.check({
					.name = name,
					.instruction = pauth::data_processing(
						operation, destination),
					.initial = initial,
					.expected = initial,
					.native = pauth::native_pauth,
					.validator =
						&pauth::validate_inserted_code,
					.validator_relation_id =
						"permitted-transformed-code/v1",
				});
				++cases;
			}
		}
	}
}

void system_alias_forms(
	test::EngineHarness &harness, std::size_t &cases)
{
	constexpr std::array immediates{8U, 10U, 24U, 25U, 26U, 27U};
	for (const unsigned immediate : immediates) {
		for (unsigned value_index = 0;
		     value_index < values.size(); ++value_index) {
			auto initial = pauth::initial_state(
				immediate * 32 + value_index);
			const unsigned destination = immediate < 16U ? 17U : 30U;
			initial.registers[destination] = values[value_index];
			const pauth::CaseName name{
				"pac-alias-i%u-v%u", immediate, value_index};
			harness.check({
				.name = name,
				.instruction = pauth::hint(immediate),
				.initial = initial,
				.expected = initial,
				.native = pauth::native_pauth,
				.validator = &pauth::validate_inserted_code,
				.validator_relation_id =
					"permitted-transformed-code/v1",
			});
			++cases;
		}
	}
}

void reserved_zero_modifier_encodings(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (unsigned operation = 8; operation < 16; ++operation) {
		for (unsigned modifier = 0; modifier < 31; ++modifier) {
			for (unsigned destination = 0;
			     destination < 32; ++destination) {
				const pauth::CaseName name{
					"pac-reserved-op%u-n%u-d%u",
					operation, modifier, destination};
				harness.check_illegal(
					name,
					pauth::data_processing(
						operation, destination, modifier),
					pauth::initial_state(
						operation * 1024 +
						modifier * 32 + destination),
					pauth::native_pauth);
				++cases;
			}
		}
	}
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (unsigned operation = 0; operation < 8; ++operation) {
		for (const bool zero_modifier : {false, true}) {
			const auto built = pauth::gadgets::Builder{}.build(
				pauth::pc,
				std::array{pauth::data_processing(
					operation | (zero_modifier ? 8U : 0U),
					3, zero_modifier ? 31U : 7U)});
			pauth::require(
				built.complete &&
				built.program.instruction_count() == 1 &&
				built.program.words().size() == 4);
			entries.insert(built.program.words().front());
		}
	}
	pauth::require(entries.size() == 16);

	const auto discard = pauth::gadgets::Builder{}.build(
		pauth::pc,
		std::array{pauth::data_processing(0, 31, 7)});
	const auto nop = pauth::gadgets::Builder{}.build(
		pauth::pc, std::array{pauth::hint(0)});
	pauth::require(discard.program.words().front() ==
		nop.program.words().front());
}

} // namespace

int main()
{
	test::EngineHarness harness;
	std::size_t cases = 0;
	register_modifier_forms(harness, cases);
	zero_modifier_forms(harness, cases);
	system_alias_forms(harness, cases);
	reserved_zero_modifier_encodings(harness, cases);
	production_specialization();
	pauth::require_counts(harness, cases);
	harness.print_summary("arm64-base-pauth-code");
	return 0;
}
