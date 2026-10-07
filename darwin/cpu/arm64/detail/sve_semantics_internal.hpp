// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/sve_semantics.hpp"
#include "cpu/arm64/detail/sve_semantics_forms.hpp"

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

/*
 * What the semantic library's sources share (sve_semantics.hpp): element
 * and predicate access at VL 16, the operand context of one instruction,
 * and the handlers each source defines for its families of forms.
 */

namespace uml::cpu::arm64::detail::sve_semantics {

inline constexpr unsigned vector_bytes = sve_vector_length;

/* A Z register's 16 bytes, with little-endian element access. */
struct Vector final {
	std::array<std::uint8_t, vector_bytes> bytes{};

	template<typename T>
	[[nodiscard]] T get(unsigned element) const noexcept
	{
		T value;
		std::memcpy(
			&value, bytes.data() + element * sizeof(T), sizeof(T));
		return value;
	}

	template<typename T>
	void set(unsigned element, T value) noexcept
	{
		std::memcpy(
			bytes.data() + element * sizeof(T), &value, sizeof(T));
	}
};

using Predicate = SvePredicate;

/* Whether element `element` of `esize` bytes is active: the mask byte of
 * its lowest byte, by its top bit. */
[[nodiscard]] inline bool active(
	const Predicate &predicate, unsigned element, unsigned esize) noexcept
{
	return (predicate[element * esize] & 0x80U) != 0U;
}

/* Writes element `element` of a predicate for elements of `esize` bytes:
 * its lowest byte's mask, and clear for its other bytes. */
inline void set_active(Predicate &predicate, unsigned element, unsigned esize,
	bool value) noexcept
{
	predicate[element * esize] = value ? 0xffU : 0x00U;
	for (unsigned byte = 1; byte < esize; ++byte)
		predicate[element * esize + byte] = 0x00U;
}

/* Every byte of a predicate as a .B element. */
[[nodiscard]] inline Predicate predicate_of_bits(std::uint32_t bits) noexcept
{
	return sve_predicate_mask(static_cast<std::uint16_t>(bits));
}

[[nodiscard]] inline std::uint32_t predicate_bits(
	const Predicate &predicate) noexcept
{
	return sve_predicate_packed(predicate);
}

/*
 * PredTest(): N, the first element `mask` makes active is active in
 * `result`; Z, no element active in `mask` is active in `result`; C, the
 * last such element is not; V clear. NZCV in CpuState's layout.
 */
[[nodiscard]] std::uint32_t predicate_test(const Predicate &mask,
	const Predicate &result, unsigned esize) noexcept;

/* The element size in bytes of a size field value. */
[[nodiscard]] constexpr unsigned esize_of(unsigned size) noexcept
{
	return 1U << (size & 3U);
}

template<unsigned Bytes>
struct UnsignedOf;
template<>
struct UnsignedOf<1> {
	using type = std::uint8_t;
};
template<>
struct UnsignedOf<2> {
	using type = std::uint16_t;
};
template<>
struct UnsignedOf<4> {
	using type = std::uint32_t;
};
template<>
struct UnsignedOf<8> {
	using type = std::uint64_t;
};
template<unsigned Bytes>
using unsigned_of = typename UnsignedOf<Bytes>::type;

/* The unsigned element type twice as wide as T, and half as wide. */
template<typename T>
using wider_of = unsigned_of<sizeof(T) * 2U>;
template<typename T>
using narrower_of = unsigned_of<sizeof(T) / 2U>;

/* Calls f.template operator()<T>() for the unsigned element type of a
 * size field value (0 for bytes through 3 for doublewords). */
template<typename F>
decltype(auto) with_size(unsigned size, F &&f)
{
	switch (size & 3U) {
	case 0:
		return f.template operator()<std::uint8_t>();
	case 1:
		return f.template operator()<std::uint16_t>();
	case 2:
		return f.template operator()<std::uint32_t>();
	default:
		return f.template operator()<std::uint64_t>();
	}
}

template<typename T>
inline constexpr unsigned elements_of = vector_bytes / sizeof(T);

template<typename T>
inline constexpr unsigned bits_of = sizeof(T) * 8U;

template<typename T>
[[nodiscard]] constexpr std::make_signed_t<T> to_signed(T value) noexcept
{
	return static_cast<std::make_signed_t<T>>(value);
}

/* Saturates a mathematical value to T, signed or unsigned. */
template<typename T>
[[nodiscard]] constexpr T saturate_signed(__int128 value) noexcept
{
	using S = std::make_signed_t<T>;
	if (value > std::numeric_limits<S>::max())
		return static_cast<T>(std::numeric_limits<S>::max());
	if (value < std::numeric_limits<S>::min())
		return static_cast<T>(std::numeric_limits<S>::min());
	return static_cast<T>(static_cast<S>(value));
}

template<typename T>
[[nodiscard]] constexpr T saturate_unsigned(__int128 value) noexcept
{
	if (value > static_cast<__int128>(std::numeric_limits<T>::max()))
		return std::numeric_limits<T>::max();
	if (value < 0)
		return 0;
	return static_cast<T>(value);
}

template<typename T>
[[nodiscard]] constexpr T saturate(__int128 value, bool is_signed) noexcept
{
	return is_signed ? saturate_signed<T>(value) :
			   saturate_unsigned<T>(value);
}

/* An element's mathematical value, signed or unsigned. */
template<typename T>
[[nodiscard]] constexpr __int128 value_of(T element, bool is_signed) noexcept
{
	return is_signed ? static_cast<__int128>(to_signed(element)) :
			   static_cast<__int128>(element);
}

/*
 * SQRDMLAH, SQRDMLSH and SQRDCMLAH's element: the accumulator `addend`
 * shifted up an element's width plus or minus twice `product`, rounded,
 * shifted back and saturated. Halving every term keeps the doubleword
 * case inside 128 bits: (a << esize) + 2p + (1 << (esize - 1)) is twice
 * (a << (esize - 1)) + p + (1 << (esize - 2)), so its floor shifted by
 * esize equals the latter's shifted by esize - 1.
 */
template<typename T>
[[nodiscard]] constexpr T rounding_doubling_accumulate(
	T addend, __int128 product, bool subtract) noexcept
{
	constexpr unsigned esize = sizeof(T) * 8U;
	const __int128 base = static_cast<__int128>(to_signed(addend)) *
		(__int128{1} << (esize - 1U));
	const __int128 sum = (subtract ? base - product : base + product) +
		(__int128{1} << (esize - 2U));
	return saturate_signed<T>(sum >> (esize - 1U));
}

/* The operands and state of the instruction being executed. */
struct Context final {
	CpuState &cpu;
	std::uint32_t word;
	const SmeEncoding &row;
	SveMemory &memory;

	[[nodiscard]] unsigned field(
		unsigned low, unsigned width) const noexcept
	{
		return (word >> low) & ((1U << width) - 1U);
	}

	[[nodiscard]] bool bit(unsigned position) const noexcept
	{
		return ((word >> position) & 1U) != 0U;
	}

	[[nodiscard]] Vector z(unsigned n) const noexcept
	{
		Vector vector;
		std::memcpy(vector.bytes.data(),
			cpu.vectors[n & 31U].bytes.data(), vector_bytes);
		return vector;
	}

	void set_z(unsigned n, const Vector &vector) noexcept
	{
		std::memcpy(cpu.vectors[n & 31U].bytes.data(),
			vector.bytes.data(), vector_bytes);
	}

	[[nodiscard]] const Predicate &p(unsigned n) const noexcept
	{
		return cpu.sve_p[n & 15U];
	}

	void set_p(unsigned n, const Predicate &predicate) noexcept
	{
		cpu.sve_p[n & 15U] = predicate;
	}

	/* A general register, XZR for 31. */
	[[nodiscard]] std::uint64_t x(unsigned n) const noexcept
	{
		return n < 31U ? cpu.registers[n] : 0U;
	}

	/* A general register, SP for 31. */
	[[nodiscard]] std::uint64_t x_or_sp(unsigned n) const noexcept
	{
		return n < 31U ? cpu.registers[n] : cpu.sp;
	}

	/* Writes a general register; 31 discards. */
	void set_x(unsigned n, std::uint64_t value) noexcept
	{
		if (n < 31U)
			cpu.registers[n] = value;
	}

	void set_x_or_sp(unsigned n, std::uint64_t value) noexcept
	{
		if (n < 31U)
			cpu.registers[n] = value;
		else
			cpu.sp = value;
	}

	/* Writes the low `bytes` of a SIMD&FP register and zeroes the rest,
	 * as a scalar write of V does. */
	void set_v_scalar(
		unsigned n, std::uint64_t value, unsigned bytes) noexcept
	{
		Vector vector;
		std::memcpy(vector.bytes.data(), &value, bytes);
		set_z(n, vector);
	}
};

using Handler = SveResult (*)(Context &);

[[nodiscard]] inline SveResult retired() noexcept
{
	return {};
}

[[nodiscard]] inline SveResult fatal() noexcept
{
	return {.event = SveEvent::fatal};
}

[[nodiscard]] inline SveResult undefined() noexcept
{
	return {.event = SveEvent::undefined};
}

/* The number of elements of `esize` bytes the predicate constraint
 * `pattern` selects at VL 16 (DecodePredCount()). */
[[nodiscard]] unsigned predicate_count(
	unsigned pattern, unsigned esize) noexcept;

/* The handler of a form, or nullptr when the library does not cover it. */
[[nodiscard]] Handler predicate_handler(std::uint16_t form) noexcept;
[[nodiscard]] Handler integer_handler(std::uint16_t form) noexcept;
[[nodiscard]] Handler wide_handler(std::uint16_t form) noexcept;
[[nodiscard]] Handler permute_handler(std::uint16_t form) noexcept;
[[nodiscard]] Handler floating_handler(std::uint16_t form) noexcept;

} // namespace uml::cpu::arm64::detail::sve_semantics
