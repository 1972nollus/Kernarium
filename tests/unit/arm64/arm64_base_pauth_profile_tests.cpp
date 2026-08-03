// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

namespace pauth = arm64_pauth_test;
namespace test = uml::emulator::tests;

namespace {

void unavailable_hint_encodings(test::EngineHarness &harness,
	std::size_t &cases)
{
	constexpr std::array immediates{
		7U, 8U, 10U, 12U, 14U,
		24U, 25U, 26U, 27U, 28U, 29U, 30U, 31U,
	};
	for (const auto immediate : immediates) {
		auto initial = pauth::initial_state(immediate);
		initial.pointer_authentication_enabled = false;
		const pauth::CaseName name{"pauth-unavailable-hint-%u", immediate};
		harness.check({
			.name = name,
			.instruction = pauth::hint(immediate),
			.initial = initial,
			.expected = initial,
			.native = pauth::native_pauth,
		});
		++cases;
	}
}

void unavailable_non_hint_encodings(test::EngineHarness &harness,
	std::size_t &cases)
{
	constexpr std::array instructions{
		pauth::data_processing(0, 3, 5),
		pauth::xpac(false, 3),
		pauth::pacga(3, 5, 7),
		pauth::authenticated_branch(false, false, false, 3, 5),
		pauth::authenticated_return(false),
		pauth::authenticated_load(false, false, 0, 5, 3),
	};
	for (std::size_t index = 0; index < instructions.size(); ++index) {
		auto initial = pauth::initial_state(
			static_cast<unsigned>(0x100 + index));
		initial.pointer_authentication_enabled = false;
		const pauth::CaseName name{
			"pauth-unavailable-non-hint-%zu", index};
		harness.check_illegal(
			name, instructions[index], initial, pauth::native_pauth);
		++cases;
	}
}

} // namespace

int main()
{
	test::EngineHarness harness{test::EngineHarnessOptions{
		.enable_pointer_authentication = false,
	}};
	std::size_t cases = 0;
	unavailable_hint_encodings(harness, cases);
	unavailable_non_hint_encodings(harness, cases);
	pauth::require_counts(harness, cases);
	harness.print_summary("arm64-base-pauth-profile");
	return 0;
}
