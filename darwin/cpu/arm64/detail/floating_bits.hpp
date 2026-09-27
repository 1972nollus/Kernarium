// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <concepts>
#include <cstdint>

namespace uml::cpu::arm64::detail::floating_bits {

/* The unsigned integer that holds an IEEE binary16, binary32 or binary64
 * encoding; its width selects the format. */
template<typename Integer>
concept Encoding = std::same_as<Integer, std::uint16_t> ||
	std::same_as<Integer, std::uint32_t> ||
	std::same_as<Integer, std::uint64_t>;

template<Encoding Integer>
inline constexpr unsigned total_bits = sizeof(Integer) * 8U;

template<Encoding Integer>
inline constexpr unsigned fraction_bits =
	sizeof(Integer) == 2U ? 10U : sizeof(Integer) == 4U ? 23U : 52U;

template<Encoding Integer>
inline constexpr unsigned exponent_bits =
	total_bits<Integer> - fraction_bits<Integer> - 1U;

template<Encoding Integer>
inline constexpr Integer sign_bit =
	Integer{1} << (total_bits<Integer> - 1U);

template<Encoding Integer>
inline constexpr Integer fraction_mask =
	(Integer{1} << fraction_bits<Integer>) - 1U;

template<Encoding Integer>
inline constexpr Integer exponent_mask =
	((Integer{1} << exponent_bits<Integer>) - 1U) <<
	fraction_bits<Integer>;

template<Encoding Integer>
inline constexpr Integer quiet_bit =
	Integer{1} << (fraction_bits<Integer> - 1U);

template<Encoding Integer>
[[nodiscard]] constexpr bool is_nan(Integer value) noexcept
{
	return (value & exponent_mask<Integer>) == exponent_mask<Integer> &&
		(value & fraction_mask<Integer>) != 0U;
}

template<Encoding Integer>
[[nodiscard]] constexpr bool is_signaling_nan(Integer value) noexcept
{
	return is_nan(value) && (value & quiet_bit<Integer>) == 0U;
}

template<Encoding Integer>
[[nodiscard]] constexpr bool is_infinity(Integer value) noexcept
{
	return (value & ~sign_bit<Integer>) == exponent_mask<Integer>;
}

template<Encoding Integer>
[[nodiscard]] constexpr bool is_zero(Integer value) noexcept
{
	return (value & ~sign_bit<Integer>) == 0U;
}

template<Encoding Integer>
[[nodiscard]] constexpr bool is_subnormal(Integer value) noexcept
{
	return (value & exponent_mask<Integer>) == 0U &&
		(value & fraction_mask<Integer>) != 0U;
}

template<Encoding Integer>
[[nodiscard]] constexpr Integer flush_subnormal(Integer value) noexcept
{
	return is_subnormal(value) ? value & sign_bit<Integer> : value;
}

} // namespace uml::cpu::arm64::detail::floating_bits
