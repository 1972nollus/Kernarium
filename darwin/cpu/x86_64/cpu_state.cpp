// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu_state.hpp"

namespace uml::cpu::x86_64 {
namespace {

[[nodiscard]] constexpr std::uint64_t mask_for_width(
	std::uint8_t width) noexcept
{
	return width == 8 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << (width * 8U)) - 1U;
}

[[nodiscard]] constexpr bool even_parity(std::uint8_t value) noexcept
{
	value ^= static_cast<std::uint8_t>(value >> 4U);
	value &= 0xfU;
	return ((0x9669U >> value) & 1U) != 0;
}

} // namespace

void materialize_flags(CpuState &cpu) noexcept
{
	const auto &deferred = cpu.deferred_flags;
	if (deferred.operation == DeferredFlagOperation::none)
		return;

	const auto mask = mask_for_width(deferred.width);
	const auto sign = std::uint64_t{1} << (deferred.width * 8U - 1U);
	const auto left = deferred.left & mask;
	const auto right = deferred.right & mask;
	const auto result = deferred.result & mask;
	constexpr auto arithmetic_flags = rflags_carry | rflags_parity |
		rflags_auxiliary | rflags_zero | rflags_sign | rflags_overflow;
	auto flags = cpu.rflags & ~arithmetic_flags;

	if (result == 0)
		flags |= rflags_zero;
	if ((result & sign) != 0)
		flags |= rflags_sign;
	if (even_parity(static_cast<std::uint8_t>(result)))
		flags |= rflags_parity;

	switch (deferred.operation) {
	case DeferredFlagOperation::add:
		if (result < left)
			flags |= rflags_carry;
		if (((~(left ^ right) & (left ^ result)) & sign) != 0)
			flags |= rflags_overflow;
		if (((left ^ right ^ result) & 0x10U) != 0)
			flags |= rflags_auxiliary;
		break;
	case DeferredFlagOperation::subtract:
		if (left < right)
			flags |= rflags_carry;
		if ((((left ^ right) & (left ^ result)) & sign) != 0)
			flags |= rflags_overflow;
		if (((left ^ right ^ result) & 0x10U) != 0)
			flags |= rflags_auxiliary;
		break;
	case DeferredFlagOperation::logical:
		break;
	case DeferredFlagOperation::none:
		break;
	}

	cpu.rflags = flags | rflags_reserved;
	cpu.deferred_flags.operation = DeferredFlagOperation::none;
}

} // namespace uml::cpu::x86_64
