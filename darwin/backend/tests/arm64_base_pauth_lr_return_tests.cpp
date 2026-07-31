// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

namespace pauth = arm64_pauth_test;
namespace test = uml::backend::tests;

namespace {

[[nodiscard]] constexpr std::uint32_t return_immediate(
	bool key_b, unsigned immediate, unsigned fixed = 31) noexcept
{
	return 0x5500'0000U | (static_cast<unsigned>(key_b) << 21) |
		((immediate & 0xffffU) << 5) | (fixed & 31U);
}

[[nodiscard]] constexpr std::uint32_t return_register(
	bool key_b, unsigned source) noexcept
{
	return 0xd65f'0be0U | (static_cast<unsigned>(key_b) << 10) |
		(source & 31U);
}

void every_immediate_form(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		for (unsigned immediate = 0;
		     immediate <= 0xffffU; ++immediate) {
			const pauth::CaseName name{
				"ret%sppc-i%u",
				key_b ? "ab" : "aa", immediate};
			pauth::check_unavailable_pauth_lr(
				harness, name,
				return_immediate(key_b, immediate),
				pauth::initial_state(
					static_cast<unsigned>(key_b) * 0x10000 +
					immediate));
			++cases;
		}
	}
}

void every_register_form(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		for (unsigned source = 0; source < 31; ++source) {
			const pauth::CaseName name{
				"ret%sppcr-m%u",
				key_b ? "ab" : "aa", source};
			pauth::check_unavailable_pauth_lr(
				harness, name,
				return_register(key_b, source),
				pauth::initial_state(
					static_cast<unsigned>(key_b) * 64 +
					source));
			++cases;
		}
	}
}

void register_31_is_classic_return(
	test::EngineHarness &harness, std::size_t &cases)
{
	constexpr std::size_t target_offset = 16;
	constexpr std::uint64_t target =
		test::engine_test_code_base + target_offset;
	for (const bool key_b : {false, true}) {
		auto initial = pauth::initial_state(
			0x20000 + static_cast<unsigned>(key_b));
		initial.registers[30] = target;
		const std::array code{
			test::ProgramWord{
				.offset = 0,
				.instruction = pauth::data_processing(
					key_b ? 1U : 0U, 30, 31)},
			test::ProgramWord{
				.offset = 4,
				.instruction = return_register(key_b, 31)},
			test::ProgramWord{
				.offset = target_offset,
				.instruction = pauth::xpac(false, 30)},
			test::ProgramWord{
				.offset = target_offset + 4,
				.instruction = 0xd400'0001U},
		};
		auto expected = initial;
		expected.registers[30] = target;
		expected.pc = target + 8;
		const pauth::CaseName name{
			"ret%sppcr-rm31-classic",
			key_b ? "ab" : "aa"};
		harness.check_program({
			.name = name,
			.code = code,
			.initial = initial,
			.expected = expected,
			.native = pauth::native_pauth,
		});
		++cases;
	}
}

void reserved_immediate_fixed_field(
	test::EngineHarness &harness, std::size_t &cases)
{
	constexpr std::array immediates{
		0U, 1U, 0x7fffU, 0x8000U, 0xfffeU, 0xffffU};
	for (const bool key_b : {false, true}) {
		for (const unsigned immediate : immediates) {
			for (unsigned fixed = 0; fixed < 31; ++fixed) {
				const pauth::CaseName name{
					"ret%sppc-reserved-i%u-r%u",
					key_b ? "ab" : "aa",
					immediate, fixed};
				harness.check_illegal(
					name,
					return_immediate(
						key_b, immediate, fixed),
					pauth::initial_state(
						0x30000 +
						static_cast<unsigned>(key_b) *
							1024 +
						fixed),
					pauth::native_pauth_lr);
				++cases;
			}
		}
	}
}

void production_unavailable_specialization()
{
	const auto illegal = pauth::gadgets::Builder{}.build(
		pauth::pc, std::array{std::uint32_t{0}});
	for (const auto instruction :
	     {return_immediate(false, 0),
	      return_immediate(true, 0),
	      return_register(false, 3),
	      return_register(true, 3)}) {
		const auto built = pauth::gadgets::Builder{}.build(
			pauth::pc, std::array{instruction});
		pauth::require(
			built.complete &&
			built.program.words().front() ==
				illegal.program.words().front());
	}
	for (const bool key_b : {false, true}) {
		const auto built = pauth::gadgets::Builder{}.build(
			pauth::pc,
			std::array{return_register(key_b, 31)});
		pauth::require(
			built.complete &&
			built.program.words().front() !=
				illegal.program.words().front());
	}
}

} // namespace

int main()
{
	test::EngineHarness harness;
	std::size_t cases = 0;
	every_immediate_form(harness, cases);
	every_register_form(harness, cases);
	register_31_is_classic_return(harness, cases);
	reserved_immediate_fixed_field(harness, cases);
	production_unavailable_specialization();
	pauth::require_counts(harness, cases);
	harness.print_summary("arm64-base-pauth-lr-return");
	return 0;
}
