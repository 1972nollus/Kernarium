// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

namespace pauth = arm64_pauth_test;
namespace test = uml::emulator::tests;

namespace {

[[nodiscard]] constexpr std::uint32_t authenticate_sp_pc(
	bool key_b, unsigned immediate, unsigned destination = 31) noexcept
{
	return 0xf380'0000U | (static_cast<unsigned>(key_b) << 21) |
		((immediate & 0xffffU) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t authenticate_sp_pc_register(
	bool key_b, unsigned source, unsigned destination = 30) noexcept
{
	return 0xdac1'9000U | (static_cast<unsigned>(key_b) << 10) |
		((source & 31U) << 5) | (destination & 31U);
}

constexpr std::array fixed_encodings{
	0xdac1'8bfeU, // PACIA171615
	0xdac1'8ffeU, // PACIB171615
	0xdac1'bbfeU, // AUTIA171615
	0xdac1'bffeU, // AUTIB171615
	0xdac1'a3feU, // PACIASPPC
	0xdac1'a7feU, // PACIBSPPC
	0xdac1'83feU, // PACNBIASPPC
	0xdac1'87feU, // PACNBIBSPPC
};

void fixed_allocated_forms(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (unsigned encoding = 0;
	     encoding < fixed_encodings.size(); ++encoding) {
		for (unsigned salt = 0; salt < 8; ++salt) {
			const pauth::CaseName name{
				"pauth-lr-fixed-%u-s%u", encoding, salt};
			pauth::check_unavailable_pauth_lr(
				harness, name, fixed_encodings[encoding],
				pauth::initial_state(encoding * 16 + salt));
			++cases;
		}
	}
}

void every_immediate_authentication(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		for (unsigned immediate = 0;
		     immediate <= 0xffffU; ++immediate) {
			const pauth::CaseName name{
				"aut%sppc-i%u",
				key_b ? "ib" : "ia", immediate};
			pauth::check_unavailable_pauth_lr(
				harness, name,
				authenticate_sp_pc(key_b, immediate),
				pauth::initial_state(
					static_cast<unsigned>(key_b) * 0x10000 +
					immediate));
			++cases;
		}
	}
}

void every_register_authentication(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		for (unsigned source = 0; source < 32; ++source) {
			const pauth::CaseName name{
				"aut%spcr-n%u",
				key_b ? "ibspp" : "iaspp", source};
			pauth::check_unavailable_pauth_lr(
				harness, name,
				authenticate_sp_pc_register(key_b, source),
				pauth::initial_state(
					static_cast<unsigned>(key_b) * 64 +
					source));
			++cases;
		}
	}
}

void pacm_feature_absent_hint(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (unsigned salt = 0; salt < 32; ++salt) {
		const auto initial = pauth::initial_state(0x20000 + salt);
		const pauth::CaseName name{"pacm-nop-s%u", salt};
		harness.check_one(
			name, 0xd503'24ffU, initial, initial,
			pauth::native_pauth_lr);
		++cases;
	}
}

void reserved_fixed_destinations(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (unsigned encoding = 0;
	     encoding < fixed_encodings.size(); ++encoding) {
		for (unsigned destination = 0;
		     destination < 32; ++destination) {
			if (destination == 30U)
				continue;
			const pauth::CaseName name{
				"pauth-lr-fixed-reserved-%u-d%u",
				encoding, destination};
			harness.check_illegal(
				name,
				(fixed_encodings[encoding] & ~31U) |
					destination,
				pauth::initial_state(
					0x30000 + encoding * 32 + destination),
				pauth::native_pauth_lr);
			++cases;
		}
	}
}

void reserved_immediate_destinations(
	test::EngineHarness &harness, std::size_t &cases)
{
	constexpr std::array immediates{
		0U, 1U, 0x7fffU, 0x8000U, 0xfffeU, 0xffffU};
	for (const bool key_b : {false, true}) {
		for (const unsigned immediate : immediates) {
			for (unsigned destination = 0;
			     destination < 31; ++destination) {
				const pauth::CaseName name{
					"aut%sppc-reserved-i%u-d%u",
					key_b ? "ib" : "ia", immediate,
					destination};
				harness.check_illegal(
					name,
					authenticate_sp_pc(
						key_b, immediate, destination),
					pauth::initial_state(
						0x40000 +
						static_cast<unsigned>(key_b) *
							1024 +
						destination),
					pauth::native_pauth_lr);
				++cases;
			}
		}
	}
}

void reserved_register_destinations(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		for (unsigned source = 0; source < 32; ++source) {
			for (unsigned destination = 0;
			     destination < 32; ++destination) {
				if (destination == 30U)
					continue;
				const pauth::CaseName name{
					"aut%spcr-reserved-n%u-d%u",
					key_b ? "ibspp" : "iaspp",
					source, destination};
				harness.check_illegal(
					name,
					authenticate_sp_pc_register(
						key_b, source, destination),
					pauth::initial_state(
						0x50000 +
						static_cast<unsigned>(key_b) *
							2048 +
						source * 32 + destination),
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
	for (const auto instruction : fixed_encodings) {
		const auto built = pauth::gadgets::Builder{}.build(
			pauth::pc, std::array{instruction});
		pauth::require(
			built.complete &&
			built.program.words().front() ==
				illegal.program.words().front());
	}
	for (const auto instruction :
	     {authenticate_sp_pc(false, 0),
	      authenticate_sp_pc_register(false, 3)}) {
		const auto built = pauth::gadgets::Builder{}.build(
			pauth::pc, std::array{instruction});
		pauth::require(
			built.complete &&
			built.program.words().front() ==
				illegal.program.words().front());
	}
	const auto pacm = pauth::gadgets::Builder{}.build(
		pauth::pc, std::array{0xd503'24ffU});
	const auto nop = pauth::gadgets::Builder{}.build(
		pauth::pc, std::array{pauth::hint(0)});
	pauth::require(pacm.program.words().front() ==
		nop.program.words().front());
}

} // namespace

int main()
{
	test::EngineHarness harness;
	std::size_t cases = 0;
	fixed_allocated_forms(harness, cases);
	every_immediate_authentication(harness, cases);
	every_register_authentication(harness, cases);
	pacm_feature_absent_hint(harness, cases);
	reserved_fixed_destinations(harness, cases);
	reserved_immediate_destinations(harness, cases);
	reserved_register_destinations(harness, cases);
	production_unavailable_specialization();
	pauth::require_counts(harness, cases);
	harness.print_summary("arm64-base-pauth-lr-data");
	return 0;
}
