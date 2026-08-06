// SPDX-License-Identifier: GPL-2.0-or-later
#include "complete_helpers.hpp"

#include <algorithm>
#include <limits>

namespace uml::cpu::arm64::simd_complete_detail {
namespace {

void record_saturation(CpuState &cpu, bool saturated)
{
	if (saturated)
		cpu.fpsr |= 1U << 27; // QC, cumulative saturation flag.
}

} // namespace

std::uint64_t saturate_signed(CpuState &cpu, SignedWide value, unsigned bits)
{
	const auto minimum = signed_minimum(bits);
	const auto maximum = signed_maximum(bits);
	const bool saturated = value < minimum || value > maximum;
	value = std::clamp(value, minimum, maximum);
	record_saturation(cpu, saturated);
	return static_cast<std::uint64_t>(value) & mask_for(bits);
}

std::uint64_t saturate_unsigned(
	CpuState &cpu, SignedWide value, unsigned bits)
{
	const auto maximum = static_cast<UnsignedWide>(mask_for(bits));
	const bool below = value < 0;
	const bool above = !below && static_cast<UnsignedWide>(value) > maximum;
	record_saturation(cpu, below || above);
	if (below)
		return 0;
	if (above)
		return mask_for(bits);
	return static_cast<std::uint64_t>(value);
}

std::uint64_t rounded_shift_right_signed(
	std::uint64_t raw, unsigned bits, unsigned shift, bool rounding)
{
	SignedWide value = as_signed(raw, bits);
	if (shift > bits)
		return !rounding && value < 0 ? mask_for(bits) : 0;
	if (rounding && shift != 0)
		value += SignedWide{1} << (shift - 1);
	if (shift >= bits)
		return value < 0 ? mask_for(bits) : 0;
	return static_cast<std::uint64_t>(value >> shift) & mask_for(bits);
}

std::uint64_t rounded_shift_right_unsigned(
	std::uint64_t value, unsigned bits, unsigned shift, bool rounding)
{
	UnsignedWide wide = value & mask_for(bits);
	if (shift > bits)
		return 0;
	if (rounding && shift != 0)
		wide += UnsignedWide{1} << (shift - 1);
	return shift >= bits ? static_cast<std::uint64_t>(wide >> bits) :
		static_cast<std::uint64_t>(wide >> shift) & mask_for(bits);
}

std::uint64_t polynomial_multiply_byte(
	std::uint64_t left, std::uint64_t right)
{
	std::uint64_t product = 0;
	for (unsigned bit = 0; bit < 8; ++bit)
		if ((right & (1U << bit)) != 0)
			product ^= left << bit;
	return product & 0xffU;
}

std::uint64_t floating_to_integer(
	long double value, bool unsigned_result,
	unsigned bits, unsigned rounding)
{
	if (std::isnan(value))
		return 0;
	switch (rounding) {
	case 0: value = std::nearbyint(value); break;
	case 1: value = std::ceil(value); break;
	case 2: value = std::floor(value); break;
	case 4: value = std::round(value); break;
	default: value = std::trunc(value); break;
	}
	if (unsigned_result) {
		const auto maximum = bits == 64 ?
			static_cast<long double>(
				std::numeric_limits<std::uint64_t>::max()) :
			static_cast<long double>(
				std::numeric_limits<std::uint32_t>::max());
		if (value <= 0)
			return 0;
		if (value >= maximum)
			return mask_for(bits);
		return static_cast<std::uint64_t>(value);
	}
	const auto minimum = bits == 64 ?
		static_cast<long double>(std::numeric_limits<std::int64_t>::min()) :
		static_cast<long double>(std::numeric_limits<std::int32_t>::min());
	const auto maximum = bits == 64 ?
		static_cast<long double>(std::numeric_limits<std::int64_t>::max()) :
		static_cast<long double>(std::numeric_limits<std::int32_t>::max());
	if (value <= minimum)
		return std::uint64_t{1} << (bits - 1);
	if (value >= maximum)
		return (std::uint64_t{1} << (bits - 1)) - 1;
	return static_cast<std::uint64_t>(static_cast<std::int64_t>(value)) &
		mask_for(bits);
}

} // namespace uml::cpu::arm64::simd_complete_detail
