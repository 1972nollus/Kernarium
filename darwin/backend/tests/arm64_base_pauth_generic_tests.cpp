// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

#include <set>

namespace pauth = arm64_pauth_test;
namespace test = uml::backend::tests;

namespace {

void every_register_encoding(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (unsigned destination = 0; destination < 32; ++destination) {
		for (unsigned source = 0; source < 32; ++source) {
			for (unsigned modifier = 0; modifier < 32; ++modifier) {
				auto initial = pauth::initial_state(
					destination * 1024 + source * 32 + modifier);
				const pauth::CaseName name{
					"pacga-d%u-n%u-m%u",
					destination, source, modifier};
				harness.check({
					.name = name,
					.instruction = pauth::pacga(
						destination, source, modifier),
					.initial = initial,
					.expected = initial,
					.native = pauth::native_pauth,
					.validator =
						&pauth::validate_generic_code,
				});
				++cases;
			}
		}
	}
}

void known_answer_across_engines(
	test::EngineHarness &harness, std::size_t &cases)
{
	auto initial = pauth::initial_state(0x5a5a);
	initial.pointer_auth_keys[4] = {
		0xec28'02d4'e0a4'88e9ULL,
		0x84be'85ce'9804'e94bULL,
	};
	initial.registers[5] = 0xfb62'3599'da6e'8127ULL;
	initial.registers[7] = 0x477d'469d'ec0b'8762ULL;
	auto expected = initial;
	expected.registers[3] = 0xc003'b939'0000'0000ULL;
	harness.check({
		.name = "pacga-known-answer",
		.instruction = pauth::pacga(3, 5, 7),
		.initial = initial,
		.expected = expected,
		.native = pauth::native_pauth,
	});
	++cases;
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const bool source_zero : {false, true}) {
		const auto built = pauth::gadgets::Builder{}.build(
			pauth::pc,
			std::array{pauth::pacga(
				3, source_zero ? 31U : 7U, 11)});
		pauth::require(
			built.complete &&
			built.program.instruction_count() == 1 &&
			built.program.words().size() == 4);
		entries.insert(built.program.words().front());
	}
	pauth::require(entries.size() == 2);
	const auto discard = pauth::gadgets::Builder{}.build(
		pauth::pc, std::array{pauth::pacga(31, 7, 11)});
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
	every_register_encoding(harness, cases);
	known_answer_across_engines(harness, cases);
	production_specialization();
	pauth::require_counts(harness, cases);
	harness.print_summary("arm64-base-pauth-generic");
	return 0;
}
