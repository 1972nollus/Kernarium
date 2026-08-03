// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

#include <set>

namespace pauth = arm64_pauth_test;
namespace test = uml::emulator::tests;

namespace {

constexpr std::array values{
	0xab55'1234'5678'9abcULL,
	0x12d5'fedc'ba98'7654ULL,
	0xff00'0000'0000'0000ULL,
	0x00ff'ffff'ffff'ffffULL,
	0x1234'8000'0000'0001ULL,
	0x0000'0000'0000'0000ULL,
};

void integer_forms(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool data : {false, true}) {
		for (const bool mte_enabled : {false, true}) {
			for (unsigned destination = 0;
			     destination < 32; ++destination) {
				for (unsigned value_index = 0;
				     value_index < values.size(); ++value_index) {
					auto initial = pauth::initial_state(
						static_cast<unsigned>(data) * 4096 +
						static_cast<unsigned>(mte_enabled) *
							2048 +
						destination * 16 + value_index);
					initial.set_mte_configuration(
						mte_enabled, 0x8001U);
					if (destination < 31U)
						initial.registers[destination] =
							values[value_index];
					auto expected = initial;
					if (destination < 31U)
						expected.registers[destination] =
							pauth::strip(
								values[value_index], data);
					const pauth::CaseName name{
						"xpac-%c-mte%u-d%u-v%u",
						data ? 'd' : 'i',
						static_cast<unsigned>(mte_enabled),
						destination, value_index};
					harness.check_one(
						name,
						pauth::xpac(data, destination),
						initial, expected,
						pauth::native_pauth);
					++cases;
				}
			}
		}
	}
}

void link_register_form(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (unsigned value_index = 0;
	     value_index < values.size(); ++value_index) {
		auto initial = pauth::initial_state(value_index);
		initial.registers[30] = values[value_index];
		auto expected = initial;
		expected.registers[30] =
			pauth::strip(values[value_index], false);
		const pauth::CaseName name{
			"xpaclri-v%u", value_index};
		harness.check_one(
			name, 0xd503'20ffU, initial, expected,
			pauth::native_pauth);
		++cases;
	}
}

void reserved_source(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool data : {false, true}) {
		for (unsigned source = 0; source < 31; ++source) {
			for (unsigned destination = 0;
			     destination < 32; ++destination) {
				const pauth::CaseName name{
					"xpac-reserved-%c-n%u-d%u",
					data ? 'd' : 'i', source, destination};
				harness.check_illegal(
					name,
					pauth::xpac(data, destination, source),
					pauth::initial_state(
						static_cast<unsigned>(data) * 1024 +
						source * 32 + destination),
					pauth::native_pauth);
				++cases;
			}
		}
	}
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const auto instruction :
	     {pauth::xpac(false, 3), pauth::xpac(true, 3),
	      std::uint32_t{0xd503'20ffU}}) {
		const auto built = pauth::gadgets::Builder{}.build(
			pauth::pc, std::array{instruction});
		pauth::require(
			built.complete &&
			built.program.instruction_count() == 1);
		entries.insert(built.program.words().front());
	}
	pauth::require(entries.size() == 3);
	const auto discard = pauth::gadgets::Builder{}.build(
		pauth::pc, std::array{pauth::xpac(false, 31)});
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
	integer_forms(harness, cases);
	link_register_form(harness, cases);
	reserved_source(harness, cases);
	production_specialization();
	pauth::require_counts(harness, cases);
	harness.print_summary("arm64-base-pauth-strip");
	return 0;
}
