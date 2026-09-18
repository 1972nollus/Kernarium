// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

namespace pauth = arm64_pauth_test;

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
	production_unavailable_specialization();
	return 0;
}
