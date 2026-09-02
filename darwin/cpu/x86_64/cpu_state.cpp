// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu_state.hpp"
#include "layout/assembly_layout.hpp"

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
	if (deferred.operation == DeferredFlagOperation::none &&
		deferred.override_mask == 0)
		return;

	auto flags = cpu.rflags;
	if (deferred.operation != DeferredFlagOperation::none) {
		const auto mask = mask_for_width(deferred.width);
		const auto sign = std::uint64_t{1} << (deferred.width * 8U - 1U);
		const auto left = deferred.left & mask;
		const auto right = deferred.right & mask;
		const auto result = deferred.result & mask;
		constexpr auto arithmetic_flags = rflags_carry | rflags_parity |
			rflags_auxiliary | rflags_zero | rflags_sign | rflags_overflow;
		const bool preserve_carry = deferred.operation ==
			DeferredFlagOperation::increment || deferred.operation ==
			DeferredFlagOperation::decrement;
		flags &= ~(preserve_carry ?
			arithmetic_flags & ~rflags_carry : arithmetic_flags);

		if (result == 0)
			flags |= rflags_zero;
		if ((result & sign) != 0)
			flags |= rflags_sign;
		if (even_parity(static_cast<std::uint8_t>(result)))
			flags |= rflags_parity;

		switch (deferred.operation) {
		case DeferredFlagOperation::add:
		case DeferredFlagOperation::increment:
		case DeferredFlagOperation::add_carry: {
			const auto adjusted_right = (right + deferred.carry) & mask;
			if (!preserve_carry &&
				(adjusted_right < right || result < left))
				flags |= rflags_carry;
			if (((~(left ^ right) & (left ^ result)) & sign) != 0)
				flags |= rflags_overflow;
			if (((left ^ right ^ result) & 0x10U) != 0)
				flags |= rflags_auxiliary;
			break;
		}
		case DeferredFlagOperation::subtract:
		case DeferredFlagOperation::decrement:
		case DeferredFlagOperation::subtract_borrow: {
			const auto adjusted_right = (right + deferred.carry) & mask;
			if (!preserve_carry &&
				(adjusted_right < right || left < adjusted_right))
				flags |= rflags_carry;
			if ((((left ^ right) & (left ^ result)) & sign) != 0)
				flags |= rflags_overflow;
			if (((left ^ right ^ result) & 0x10U) != 0)
				flags |= rflags_auxiliary;
			break;
		}
		case DeferredFlagOperation::logical:
			break;
		case DeferredFlagOperation::none:
			break;
		}
	}

	const auto override_mask = static_cast<std::uint64_t>(
		deferred.override_mask);
	flags = (flags & ~override_mask) |
		(static_cast<std::uint64_t>(deferred.override_value) & override_mask);
	cpu.rflags = flags | rflags_reserved;
	cpu.deferred_flags.operation = DeferredFlagOperation::none;
	cpu.deferred_flags.override_mask = 0;
	cpu.deferred_flags.override_value = 0;
}

} // namespace uml::cpu::x86_64
