// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../cpu_state.hpp"

#include <array>
#include <cstdint>

/*
 * The QARMA5 core is adapted from QEMU's target/arm/tcg/pauth_helper.c at
 * commit 9f4d05a21f1d3a01c136979f4b60b7b02c60e821:
 *
 *   ARM v8.3-PAuth Operations
 *   Copyright (c) 2019 Linaro, Ltd.
 *
 * QEMU distributes that implementation under LGPL-2.1-or-later. This
 * adaptation is distributed under GPL-2.0-or-later as permitted by section 3
 * of LGPL v2.1. The surrounding address-format code specializes the
 * architectural algorithm to this emulator's 47-bit EL0 virtual-address
 * profile and its existing instruction/data TBI contract.
 */

namespace uml::backend::arm64::detail {

enum class PointerAuthOperation : unsigned {
	sign_instruction_a,
	sign_instruction_b,
	sign_data_a,
	sign_data_b,
	authenticate_instruction_a,
	authenticate_instruction_b,
	authenticate_data_a,
	authenticate_data_b,
	generic,
	strip_instruction,
	strip_data,
};

[[nodiscard]] constexpr std::uint64_t
extract_bits(std::uint64_t value, unsigned offset, unsigned width) noexcept
{
	if (width == 64U)
		return value;
	return (value >> offset) & ((std::uint64_t{1} << width) - 1U);
}

[[nodiscard]] constexpr std::uint64_t
bit_mask(unsigned offset, unsigned width) noexcept
{
	if (width == 0U)
		return 0;
	if (width == 64U)
		return ~std::uint64_t{0};
	return ((std::uint64_t{1} << width) - 1U) << offset;
}

[[nodiscard]] constexpr std::uint64_t
deposit_bits(std::uint64_t original, unsigned offset, unsigned width,
	     std::uint64_t value) noexcept
{
	const auto mask = bit_mask(offset, width);
	return (original & ~mask) | ((value << offset) & mask);
}

[[nodiscard]] constexpr std::uint64_t
sign_extract(std::uint64_t value, unsigned offset, unsigned width) noexcept
{
	const auto extracted = extract_bits(value, offset, width);
	if (width == 64U ||
	    (extracted & (std::uint64_t{1} << (width - 1U))) == 0)
		return extracted;
	return extracted | ~bit_mask(0, width);
}

[[nodiscard]] constexpr std::uint64_t
cell_shuffle(std::uint64_t input) noexcept
{
	std::uint64_t output = 0;
	output |= extract_bits(input, 52, 4);
	output |= extract_bits(input, 24, 4) << 4;
	output |= extract_bits(input, 44, 4) << 8;
	output |= extract_bits(input, 0, 4) << 12;
	output |= extract_bits(input, 28, 4) << 16;
	output |= extract_bits(input, 48, 4) << 20;
	output |= extract_bits(input, 4, 4) << 24;
	output |= extract_bits(input, 40, 4) << 28;
	output |= extract_bits(input, 32, 4) << 32;
	output |= extract_bits(input, 12, 4) << 36;
	output |= extract_bits(input, 56, 4) << 40;
	output |= extract_bits(input, 20, 4) << 44;
	output |= extract_bits(input, 8, 4) << 48;
	output |= extract_bits(input, 36, 4) << 52;
	output |= extract_bits(input, 16, 4) << 56;
	output |= extract_bits(input, 60, 4) << 60;
	return output;
}

[[nodiscard]] constexpr std::uint64_t
cell_inverse_shuffle(std::uint64_t input) noexcept
{
	std::uint64_t output = 0;
	output |= extract_bits(input, 12, 4);
	output |= extract_bits(input, 24, 4) << 4;
	output |= extract_bits(input, 48, 4) << 8;
	output |= extract_bits(input, 36, 4) << 12;
	output |= extract_bits(input, 56, 4) << 16;
	output |= extract_bits(input, 44, 4) << 20;
	output |= extract_bits(input, 4, 4) << 24;
	output |= extract_bits(input, 16, 4) << 28;
	output |= input & bit_mask(32, 4);
	output |= extract_bits(input, 52, 4) << 36;
	output |= extract_bits(input, 28, 4) << 40;
	output |= extract_bits(input, 8, 4) << 44;
	output |= extract_bits(input, 20, 4) << 48;
	output |= extract_bits(input, 0, 4) << 52;
	output |= extract_bits(input, 40, 4) << 56;
	output |= input & bit_mask(60, 4);
	return output;
}

[[nodiscard]] constexpr std::uint64_t
substitute_cells(std::uint64_t input) noexcept
{
	constexpr std::array<std::uint8_t, 16> substitution{
		0xb, 0x6, 0x8, 0xf, 0xc, 0x0, 0x9, 0xe,
		0x3, 0x7, 0x4, 0x5, 0xd, 0x2, 0x1, 0xa,
	};
	std::uint64_t output = 0;
	for (unsigned bit = 0; bit < 64; bit += 4)
		output |= static_cast<std::uint64_t>(
				  substitution[(input >> bit) & 0xfU])
			  << bit;
	return output;
}

[[nodiscard]] constexpr std::uint64_t
inverse_substitute_cells(std::uint64_t input) noexcept
{
	constexpr std::array<std::uint8_t, 16> substitution{
		0x5, 0xe, 0xd, 0x8, 0xa, 0xb, 0x1, 0x9,
		0x2, 0x6, 0xf, 0x0, 0x4, 0xc, 0x7, 0x3,
	};
	std::uint64_t output = 0;
	for (unsigned bit = 0; bit < 64; bit += 4)
		output |= static_cast<std::uint64_t>(
				  substitution[(input >> bit) & 0xfU])
			  << bit;
	return output;
}

[[nodiscard]] constexpr unsigned
rotate_cell(unsigned cell, unsigned amount) noexcept
{
	cell |= cell << 4;
	return (cell >> (4U - amount)) & 0xfU;
}

[[nodiscard]] constexpr std::uint64_t
mix_cells(std::uint64_t input) noexcept
{
	std::uint64_t output = 0;
	for (unsigned bit = 0; bit < 16; bit += 4) {
		const auto i0 = static_cast<unsigned>(extract_bits(input, bit, 4));
		const auto i4 =
			static_cast<unsigned>(extract_bits(input, bit + 16, 4));
		const auto i8 =
			static_cast<unsigned>(extract_bits(input, bit + 32, 4));
		const auto ic =
			static_cast<unsigned>(extract_bits(input, bit + 48, 4));
		const auto t0 =
			rotate_cell(i8, 1) ^ rotate_cell(i4, 2) ^
			rotate_cell(i0, 1);
		const auto t1 =
			rotate_cell(ic, 1) ^ rotate_cell(i4, 1) ^
			rotate_cell(i0, 2);
		const auto t2 =
			rotate_cell(ic, 2) ^ rotate_cell(i8, 1) ^
			rotate_cell(i0, 1);
		const auto t3 =
			rotate_cell(ic, 1) ^ rotate_cell(i8, 2) ^
			rotate_cell(i4, 1);
		output |= static_cast<std::uint64_t>(t3) << bit;
		output |= static_cast<std::uint64_t>(t2) << (bit + 16);
		output |= static_cast<std::uint64_t>(t1) << (bit + 32);
		output |= static_cast<std::uint64_t>(t0) << (bit + 48);
	}
	return output;
}

[[nodiscard]] constexpr std::uint64_t
rotate_tweak_cell(std::uint64_t cell) noexcept
{
	return (cell >> 1) | (((cell ^ (cell >> 1)) & 1U) << 3);
}

[[nodiscard]] constexpr std::uint64_t
tweak_shuffle(std::uint64_t input) noexcept
{
	std::uint64_t output = 0;
	output |= extract_bits(input, 16, 4);
	output |= extract_bits(input, 20, 4) << 4;
	output |= rotate_tweak_cell(extract_bits(input, 24, 4)) << 8;
	output |= extract_bits(input, 28, 4) << 12;
	output |= rotate_tweak_cell(extract_bits(input, 44, 4)) << 16;
	output |= extract_bits(input, 8, 4) << 20;
	output |= extract_bits(input, 12, 4) << 24;
	output |= rotate_tweak_cell(extract_bits(input, 32, 4)) << 28;
	output |= extract_bits(input, 48, 4) << 32;
	output |= extract_bits(input, 52, 4) << 36;
	output |= extract_bits(input, 56, 4) << 40;
	output |= rotate_tweak_cell(extract_bits(input, 60, 4)) << 44;
	output |= rotate_tweak_cell(extract_bits(input, 0, 4)) << 48;
	output |= extract_bits(input, 4, 4) << 52;
	output |= rotate_tweak_cell(extract_bits(input, 40, 4)) << 56;
	output |= rotate_tweak_cell(extract_bits(input, 36, 4)) << 60;
	return output;
}

[[nodiscard]] constexpr std::uint64_t
inverse_rotate_tweak_cell(std::uint64_t cell) noexcept
{
	return ((cell << 1) & 0xfU) | ((cell & 1U) ^ (cell >> 3));
}

[[nodiscard]] constexpr std::uint64_t
inverse_tweak_shuffle(std::uint64_t input) noexcept
{
	std::uint64_t output = 0;
	output |= inverse_rotate_tweak_cell(extract_bits(input, 48, 4));
	output |= extract_bits(input, 52, 4) << 4;
	output |= extract_bits(input, 20, 4) << 8;
	output |= extract_bits(input, 24, 4) << 12;
	output |= extract_bits(input, 0, 4) << 16;
	output |= extract_bits(input, 4, 4) << 20;
	output |= inverse_rotate_tweak_cell(extract_bits(input, 8, 4)) << 24;
	output |= extract_bits(input, 12, 4) << 28;
	output |= inverse_rotate_tweak_cell(extract_bits(input, 28, 4)) << 32;
	output |= inverse_rotate_tweak_cell(extract_bits(input, 60, 4)) << 36;
	output |= inverse_rotate_tweak_cell(extract_bits(input, 56, 4)) << 40;
	output |= inverse_rotate_tweak_cell(extract_bits(input, 16, 4)) << 44;
	output |= extract_bits(input, 32, 4) << 48;
	output |= extract_bits(input, 36, 4) << 52;
	output |= extract_bits(input, 40, 4) << 56;
	output |= inverse_rotate_tweak_cell(extract_bits(input, 44, 4)) << 60;
	return output;
}

[[nodiscard]] constexpr std::uint64_t
qarma5(std::uint64_t data, std::uint64_t modifier,
       const PointerAuthKey &key) noexcept
{
	constexpr std::array<std::uint64_t, 5> round_constants{
		0x0000'0000'0000'0000ULL,
		0x1319'8a2e'0370'7344ULL,
		0xa409'3822'299f'31d0ULL,
		0x082e'fa98'ec4e'6c89ULL,
		0x4528'21e6'38d0'1377ULL,
	};
	constexpr std::uint64_t alpha = 0xc0ac'29b7'c97c'50ddULL;
	const auto key0 = key.high;
	const auto key1 = key.low;
	const auto modified_key0 =
		(key0 << 63) | ((key0 >> 1) ^ (key0 >> 63));
	auto running_modifier = modifier;
	auto working = data ^ key0;

	for (unsigned round = 0; round <= 4; ++round) {
		working ^= key1 ^ running_modifier;
		working ^= round_constants[round];
		if (round != 0)
			working = mix_cells(cell_shuffle(working));
		working = substitute_cells(working);
		running_modifier = tweak_shuffle(running_modifier);
	}
	working ^= modified_key0 ^ running_modifier;
	working = substitute_cells(mix_cells(cell_shuffle(working)));
	working = mix_cells(cell_shuffle(working));
	working ^= key1;
	working = inverse_substitute_cells(cell_inverse_shuffle(working));
	working = cell_inverse_shuffle(mix_cells(working));
	working ^= key0 ^ running_modifier;
	for (unsigned round = 0; round <= 4; ++round) {
		working = inverse_substitute_cells(working);
		if (round < 4)
			working = cell_inverse_shuffle(mix_cells(working));
		running_modifier = inverse_tweak_shuffle(running_modifier);
		working ^= round_constants[4U - round];
		working ^= key1 ^ running_modifier;
		working ^= alpha;
	}
	return working ^ modified_key0;
}

/* Independent known-answer vector from the QARMA paper, also cross-checked
 * against the attributed QEMU implementation. */
static_assert(qarma5(0xfb62'3599'da6e'8127ULL,
		     0x477d'469d'ec0b'8762ULL,
		     PointerAuthKey{0xec28'02d4'e0a4'88e9ULL,
				    0x84be'85ce'9804'e94bULL}) ==
	      0xc003'b939'99b3'3765ULL);

struct PointerAuthAddressParameters final {
	unsigned bottom_bit{47};
	unsigned top_bit{64};
	bool tbi{};
};

[[nodiscard]] constexpr PointerAuthAddressParameters
pointer_auth_address_parameters(std::uint64_t pointer, bool data) noexcept
{
	const bool upper_range =
		(pointer & (std::uint64_t{1} << 55)) != 0;
	const bool tbi = data && !upper_range;
	return {47U, tbi ? 56U : 64U, tbi};
}

[[nodiscard]] constexpr std::uint64_t
original_pointer(std::uint64_t pointer, bool data) noexcept
{
	const auto parameters =
		pointer_auth_address_parameters(pointer, data);
	const auto extension = sign_extract(pointer, 55, 1);
	return deposit_bits(pointer, parameters.bottom_bit,
			    parameters.top_bit - parameters.bottom_bit,
			    extension);
}

[[nodiscard]] constexpr std::uint64_t
add_pointer_authentication(std::uint64_t pointer, std::uint64_t modifier,
			   const PointerAuthKey &key, bool data) noexcept
{
	const auto parameters =
		pointer_auth_address_parameters(pointer, data);
	const auto extension = parameters.tbi ?
		sign_extract(pointer, 55, 1) :
		sign_extract(pointer, 63, 1);
	const auto extended_pointer =
		deposit_bits(pointer, parameters.bottom_bit,
			     parameters.top_bit - parameters.bottom_bit,
			     extension);
	auto code = qarma5(extended_pointer, modifier, key);
	const auto extension_test =
		sign_extract(pointer, parameters.bottom_bit,
			     parameters.top_bit - parameters.bottom_bit);
	if (extension_test != 0 &&
	    extension_test != ~std::uint64_t{0})
		code ^= std::uint64_t{1} << (parameters.top_bit - 2U);

	if (parameters.tbi) {
		pointer &= ~bit_mask(parameters.bottom_bit,
				    56U - parameters.bottom_bit);
		code &= bit_mask(parameters.bottom_bit,
				 55U - parameters.bottom_bit);
	} else {
		pointer &= bit_mask(0, parameters.bottom_bit);
		code &= ~(bit_mask(55, 1) |
			  bit_mask(0, parameters.bottom_bit));
	}
	return code | (extension & bit_mask(55, 1)) | pointer;
}

[[nodiscard]] constexpr std::uint64_t
authenticate_pointer(std::uint64_t pointer, std::uint64_t modifier,
		     const PointerAuthKey &key, bool data,
		     unsigned key_number) noexcept
{
	const auto parameters =
		pointer_auth_address_parameters(pointer, data);
	const auto original = original_pointer(pointer, data);
	const auto code = qarma5(original, modifier, key);
	const auto difference =
		(code ^ pointer) & ~bit_mask(55, 1);
	if (extract_bits(difference, parameters.bottom_bit,
			 parameters.top_bit - parameters.bottom_bit) != 0) {
		const auto error =
			(key_number << 1U) | (key_number ^ 1U);
		return deposit_bits(original,
				    parameters.tbi ? 53U : 61U, 2U,
				    error);
	}
	return original;
}

[[nodiscard]] constexpr std::uint64_t
generic_pointer_authentication(std::uint64_t source,
			       std::uint64_t modifier,
			       const PointerAuthKey &key) noexcept
{
	return qarma5(source, modifier, key) & 0xffff'ffff'0000'0000ULL;
}

[[nodiscard]] constexpr bool
pointer_auth_operation_valid(PointerAuthOperation operation) noexcept
{
	return static_cast<unsigned>(operation) <=
		static_cast<unsigned>(PointerAuthOperation::strip_data);
}

[[nodiscard]] constexpr std::uint64_t
execute_pointer_auth(const CpuState &cpu, std::uint64_t value,
		     std::uint64_t modifier,
		     PointerAuthOperation operation) noexcept
{
	const auto encoded = static_cast<unsigned>(operation);
	if (encoded <= 3U) {
		const bool data = encoded >= 2U;
		return add_pointer_authentication(
			value, modifier, cpu.pointer_auth_keys[encoded], data);
	}
	if (encoded <= 7U) {
		const unsigned key = encoded - 4U;
		const bool data = key >= 2U;
		return authenticate_pointer(
			value, modifier, cpu.pointer_auth_keys[key], data,
			key & 1U);
	}
	if (operation == PointerAuthOperation::generic)
		return generic_pointer_authentication(
			value, modifier, cpu.pointer_auth_keys[4]);
	if (operation == PointerAuthOperation::strip_instruction)
		return original_pointer(value, false);
	if (operation == PointerAuthOperation::strip_data)
		return original_pointer(value, true);
	return value;
}

} // namespace uml::backend::arm64::detail
