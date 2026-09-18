// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

namespace pauth = arm64_pauth_test;

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
	production_unavailable_specialization();
	return 0;
}
