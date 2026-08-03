// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace arm64_bfloat_arithmetic_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

inline constexpr std::uint32_t fpcr_fiz = 1U << 0;
inline constexpr std::uint32_t fpcr_ah = 1U << 1;
inline constexpr std::uint32_t fpcr_ebf = 1U << 13;
inline constexpr std::uint32_t fpcr_fz = 1U << 24;
inline constexpr std::uint32_t fpcr_dn = 1U << 25;

inline constexpr std::uint32_t fpsr_invalid = 1U << 0;
inline constexpr std::uint32_t fpsr_overflow = 1U << 2;
inline constexpr std::uint32_t fpsr_underflow = 1U << 3;
inline constexpr std::uint32_t fpsr_inexact = 1U << 4;
inline constexpr std::uint32_t fpsr_input_denormal = 1U << 7;

class BigUnsigned final {
public:
	constexpr BigUnsigned(std::uint64_t value = 0U) noexcept
	{
		limbs_[0] = value;
	}

	[[nodiscard]] bool zero() const noexcept
	{
		for (const auto limb : limbs_)
			if (limb != 0U)
				return false;
		return true;
	}

	[[nodiscard]] unsigned bit_length() const noexcept
	{
		for (unsigned index = limbs_.size(); index-- > 0U;)
			if (limbs_[index] != 0U)
				return index * 64U +
					std::bit_width(limbs_[index]);
		return 0U;
	}

	[[nodiscard]] int compare(const BigUnsigned &other) const noexcept
	{
		for (unsigned index = limbs_.size(); index-- > 0U;) {
			if (limbs_[index] < other.limbs_[index])
				return -1;
			if (limbs_[index] > other.limbs_[index])
				return 1;
		}
		return 0;
	}

	[[nodiscard]] BigUnsigned shifted_left(unsigned shift) const
	{
		BigUnsigned result;
		const unsigned whole = shift / 64U;
		const unsigned partial = shift % 64U;
		for (unsigned source = 0U; source < limbs_.size();
		     ++source) {
			if (limbs_[source] == 0U)
				continue;
			const unsigned destination = source + whole;
			if (destination >= limbs_.size())
				std::abort();
			result.limbs_[destination] |=
				limbs_[source] << partial;
			if (partial != 0U) {
				if (destination + 1U >= limbs_.size() &&
				    (limbs_[source] >> (64U - partial)) != 0U)
					std::abort();
				if (destination + 1U < limbs_.size())
					result.limbs_[destination + 1U] |=
						limbs_[source] >>
						(64U - partial);
			}
		}
		return result;
	}

	[[nodiscard]] BigUnsigned shifted_right(unsigned shift) const noexcept
	{
		BigUnsigned result;
		const unsigned whole = shift / 64U;
		const unsigned partial = shift % 64U;
		if (whole >= limbs_.size())
			return result;
		for (unsigned destination = 0U;
		     destination + whole < limbs_.size(); ++destination) {
			const unsigned source = destination + whole;
			result.limbs_[destination] =
				limbs_[source] >> partial;
			if (partial != 0U &&
			    source + 1U < limbs_.size())
				result.limbs_[destination] |=
					limbs_[source + 1U] <<
					(64U - partial);
		}
		return result;
	}

	[[nodiscard]] bool any_low_bits(unsigned count) const noexcept
	{
		const unsigned whole = count / 64U;
		const unsigned partial = count % 64U;
		for (unsigned index = 0U;
		     index < std::min<unsigned>(whole, limbs_.size());
		     ++index)
			if (limbs_[index] != 0U)
				return true;
		if (partial != 0U && whole < limbs_.size() &&
		    (limbs_[whole] &
		     ((std::uint64_t{1} << partial) - 1U)) != 0U)
			return true;
		return false;
	}

	[[nodiscard]] bool bit(unsigned index) const noexcept
	{
		const unsigned limb = index / 64U;
		return limb < limbs_.size() &&
			((limbs_[limb] >> (index % 64U)) & 1U) != 0U;
	}

	[[nodiscard]] bool odd() const noexcept
	{
		return (limbs_[0] & 1U) != 0U;
	}

	void set_odd() noexcept
	{
		limbs_[0] |= 1U;
	}

	void increment()
	{
		for (auto &limb : limbs_) {
			if (++limb != 0U)
				return;
		}
		std::abort();
	}

	[[nodiscard]] std::uint64_t low64() const noexcept
	{
		return limbs_[0];
	}

	friend BigUnsigned add(const BigUnsigned &left,
		const BigUnsigned &right)
	{
		BigUnsigned result;
		unsigned __int128 carry{};
		for (unsigned index = 0U; index < result.limbs_.size();
		     ++index) {
			const unsigned __int128 sum =
				static_cast<unsigned __int128>(
					left.limbs_[index]) +
				right.limbs_[index] + carry;
			result.limbs_[index] =
				static_cast<std::uint64_t>(sum);
			carry = sum >> 64U;
		}
		if (carry != 0U)
			std::abort();
		return result;
	}

	friend BigUnsigned subtract(const BigUnsigned &left,
		const BigUnsigned &right)
	{
		if (left.compare(right) < 0)
			std::abort();
		BigUnsigned result;
		std::uint64_t borrow{};
		for (unsigned index = 0U; index < result.limbs_.size();
		     ++index) {
			const auto subtrahend =
				static_cast<unsigned __int128>(
					right.limbs_[index]) + borrow;
			result.limbs_[index] =
				static_cast<std::uint64_t>(
					static_cast<unsigned __int128>(
						left.limbs_[index]) -
					subtrahend);
			borrow = static_cast<unsigned __int128>(
				left.limbs_[index]) < subtrahend;
		}
		if (borrow != 0U)
			std::abort();
		return result;
	}

	friend BigUnsigned multiply(const BigUnsigned &left,
		const BigUnsigned &right)
	{
		BigUnsigned result;
		for (unsigned first = 0U; first < result.limbs_.size();
		     ++first) {
			unsigned __int128 carry{};
			for (unsigned second = 0U;
			     first + second < result.limbs_.size();
			     ++second) {
				const unsigned index = first + second;
				const unsigned __int128 product =
					static_cast<unsigned __int128>(
						left.limbs_[first]) *
						right.limbs_[second] +
					result.limbs_[index] + carry;
				result.limbs_[index] =
					static_cast<std::uint64_t>(product);
				carry = product >> 64U;
			}
			if (carry != 0U)
				std::abort();
		}
		return result;
	}

private:
	std::array<std::uint64_t, 12> limbs_{};
};

enum class FloatingClass : std::uint8_t {
	finite,
	infinity,
	nan,
};

struct BinaryValue final {
	FloatingClass type{FloatingClass::finite};
	bool negative{};
	bool signaling{};
	BigUnsigned magnitude{};
	int exponent{};
	std::uint32_t raw{};
};

struct ExactValue final {
	bool negative{};
	BigUnsigned magnitude{};
	int exponent{};
};

struct RoundedSingle final {
	std::uint32_t bits{};
	std::uint32_t flags{};
};

struct ExpectedCounts final {
	std::size_t cases{};
	std::size_t jit{};
	std::size_t feature_skips{};

	void add(engine_tests::NativeRequirements requirements,
		bool invalid_encoding = false)
	{
		++cases;
		if (invalid_encoding || engine_tests::host_supports(requirements))
			++jit;
		else
			++feature_skips;
	}
};

[[noreturn]] inline void fail(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	std::abort();
}

inline void require(bool condition, const char *message)
{
	if (!condition)
		fail(message);
}

inline void require_counts(const engine_tests::EngineHarness &harness,
	const ExpectedCounts &expected)
{
	const auto &actual = harness.counts();
	require(actual.interpreter == expected.cases,
		"unexpected interpreter case count");
	require(actual.gadgets == expected.cases,
		"unexpected gadget case count");
	require(actual.jit == expected.jit,
		"unexpected JIT case count");
	require(actual.jit_skipped_host_feature == expected.feature_skips,
		"unexpected JIT host-feature skip count");
	require(actual.jit_skipped == expected.feature_skips,
		"unexpected total JIT skip count");
	require(actual.jit_unavailable == 0U,
		"JIT engine was unavailable");
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0U)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0U; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0U;
		     byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] =
				static_cast<std::byte>(seed * 61U + reg * 31U +
					byte * 19U + 7U);
	for (unsigned reg = 0U; reg < cpu.registers.size(); ++reg)
		cpu.registers[reg] = 0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(reg + seed) *
				0x0101'0101'0101'0101ULL);
	cpu.sp = 0xfeed'face'cafe'bee0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpsr = (seed & 1U) != 0U ? 1U << 27 : 0U;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	cpu.set_mte_configuration(true, 0xa55aU);
	cpu.set_mte_generator(true, 0x1357U, 9U,
		0x3141'5926'5358'9793ULL);
	return cpu;
}

inline void write_bfloat(std::array<std::byte, 16> &bytes,
	unsigned lane, std::uint16_t bits)
{
	bytes[lane * 2U] = static_cast<std::byte>(bits & 0xffU);
	bytes[lane * 2U + 1U] = static_cast<std::byte>(bits >> 8U);
}

[[nodiscard]] inline std::uint16_t read_bfloat(
	const std::array<std::byte, 16> &bytes, unsigned lane)
{
	return static_cast<std::uint16_t>(
		std::to_integer<unsigned>(bytes[lane * 2U]) |
		(std::to_integer<unsigned>(bytes[lane * 2U + 1U]) << 8U));
}

inline void write_single(std::array<std::byte, 16> &bytes,
	unsigned lane, std::uint32_t bits)
{
	for (unsigned byte = 0U; byte < 4U; ++byte)
		bytes[lane * 4U + byte] =
			static_cast<std::byte>(bits >> (byte * 8U));
}

[[nodiscard]] inline std::uint32_t read_single(
	const std::array<std::byte, 16> &bytes, unsigned lane)
{
	std::uint32_t result{};
	for (unsigned byte = 0U; byte < 4U; ++byte)
		result |= std::to_integer<std::uint32_t>(
			bytes[lane * 4U + byte]) << (byte * 8U);
	return result;
}

[[nodiscard]] constexpr std::uint16_t bfloat(float value) noexcept
{
	return static_cast<std::uint16_t>(
		std::bit_cast<std::uint32_t>(value) >> 16U);
}

[[nodiscard]] inline BinaryValue unpack_single(std::uint32_t bits,
	bool flush_input, bool &flushed)
{
	const bool negative = (bits >> 31U) != 0U;
	const unsigned exponent = (bits >> 23U) & 0xffU;
	const unsigned fraction = bits & 0x7f'ffffU;
	if (exponent == 0xffU)
		return {fraction == 0U ? FloatingClass::infinity :
			FloatingClass::nan, negative,
			fraction != 0U && (fraction & 0x40'0000U) == 0U,
			0, 0, bits};
	if (exponent == 0U) {
		if (fraction == 0U || flush_input) {
			flushed |= fraction != 0U;
			return {FloatingClass::finite, negative, false, 0, 0,
				bits};
		}
		return {FloatingClass::finite, negative, false, fraction,
			-149, bits};
	}
	return {FloatingClass::finite, negative, false,
		0x80'0000U | fraction,
		static_cast<int>(exponent) - 127 - 23, bits};
}

[[nodiscard]] inline BinaryValue unpack_bfloat(std::uint16_t bits,
	bool flush_input, bool &flushed)
{
	const bool negative = (bits >> 15U) != 0U;
	const unsigned exponent = (bits >> 7U) & 0xffU;
	const unsigned fraction = bits & 0x7fU;
	if (exponent == 0xffU)
		return {fraction == 0U ? FloatingClass::infinity :
			FloatingClass::nan, negative,
			fraction != 0U && (fraction & 0x40U) == 0U,
			0, 0, static_cast<std::uint32_t>(bits) << 16U};
	if (exponent == 0U) {
		if (fraction == 0U || flush_input) {
			flushed |= fraction != 0U;
			return {FloatingClass::finite, negative, false, 0, 0,
				static_cast<std::uint32_t>(bits) << 16U};
		}
		return {FloatingClass::finite, negative, false, fraction,
			-133, static_cast<std::uint32_t>(bits) << 16U};
	}
	return {FloatingClass::finite, negative, false, 0x80U | fraction,
		static_cast<int>(exponent) - 127 - 7,
		static_cast<std::uint32_t>(bits) << 16U};
}

[[nodiscard]] inline ExactValue exact(const BinaryValue &value)
{
	return {value.negative, value.magnitude, value.exponent};
}

[[nodiscard]] inline ExactValue multiply_exact(
	const BinaryValue &left, const BinaryValue &right)
{
	return {left.negative != right.negative,
		multiply(left.magnitude, right.magnitude),
		left.exponent + right.exponent};
}

[[nodiscard]] inline ExactValue add_exact(const ExactValue &left,
	const ExactValue &right, bool cancellation_negative)
{
	if (left.magnitude.zero())
		return right.magnitude.zero() ?
			ExactValue{left.negative == right.negative ?
				left.negative : cancellation_negative, 0, 0} : right;
	if (right.magnitude.zero())
		return left;
	const int exponent = std::min(left.exponent, right.exponent);
	const auto left_aligned = left.magnitude.shifted_left(
		static_cast<unsigned>(left.exponent - exponent));
	const auto right_aligned = right.magnitude.shifted_left(
		static_cast<unsigned>(right.exponent - exponent));
	if (left.negative == right.negative)
		return {left.negative,
			add(left_aligned, right_aligned), exponent};
	const int order = left_aligned.compare(right_aligned);
	if (order == 0)
		return {cancellation_negative, 0, 0};
	if (order > 0)
		return {left.negative,
			subtract(left_aligned, right_aligned), exponent};
	return {right.negative,
		subtract(right_aligned, left_aligned), exponent};
}

[[nodiscard]] inline RoundedSingle round_single(const ExactValue &value,
	unsigned rounding, bool flush_output, bool fixed_round_to_odd,
	bool report_flags)
{
	if (value.magnitude.zero())
		return {static_cast<std::uint32_t>(value.negative) << 31U, 0U};
	const unsigned source_bits = value.magnitude.bit_length();
	int top_exponent =
		value.exponent + static_cast<int>(source_bits) - 1;
	if (fixed_round_to_odd && top_exponent < -126)
		return {static_cast<std::uint32_t>(value.negative) << 31U, 0U};
	if (flush_output && top_exponent < -126) {
		const std::uint32_t flags = report_flags ?
			fpsr_underflow | fpsr_inexact : 0U;
		return {static_cast<std::uint32_t>(value.negative) << 31U,
			flags};
	}

	const int target_exponent =
		top_exponent >= -126 ? top_exponent - 23 : -149;
	const int signed_shift = target_exponent - value.exponent;
	BigUnsigned retained = value.magnitude;
	unsigned shift{};
	if (signed_shift > 0) {
		shift = static_cast<unsigned>(signed_shift);
		retained = value.magnitude.shifted_right(shift);
	} else if (signed_shift < 0) {
		retained = value.magnitude.shifted_left(
			static_cast<unsigned>(-signed_shift));
	}
	const bool inexact =
		shift != 0U && value.magnitude.any_low_bits(shift);
	if (inexact) {
		if (fixed_round_to_odd) {
			retained.set_odd();
		} else {
			bool increment{};
			if (rounding == 0U) {
				const bool halfway_bit =
					value.magnitude.bit(shift - 1U);
				const bool below_halfway =
					value.magnitude.any_low_bits(
						shift - 1U);
				increment = halfway_bit &&
					(below_halfway || retained.odd());
			} else if (rounding == 1U) {
				increment = !value.negative;
			} else if (rounding == 2U) {
				increment = value.negative;
			}
			if (increment)
				retained.increment();
		}
	}

	if (top_exponent >= -126 &&
	    retained.compare(BigUnsigned{1}.shifted_left(24U)) == 0) {
		retained = retained.shifted_right(1U);
		++top_exponent;
	}
	if (top_exponent >= 128) {
		const bool infinity = fixed_round_to_odd || rounding == 0U ||
			(rounding == 1U && !value.negative) ||
			(rounding == 2U && value.negative);
		const std::uint32_t result =
			(static_cast<std::uint32_t>(value.negative) << 31U) |
			(infinity ? 0x7f80'0000U : 0x7f7f'ffffU);
		const std::uint32_t flags = report_flags ?
			fpsr_overflow | fpsr_inexact : 0U;
		return {result, flags};
	}

	unsigned exponent_field{};
	std::uint32_t fraction{};
	if (top_exponent >= -126) {
		exponent_field = static_cast<unsigned>(top_exponent + 127);
		fraction = static_cast<std::uint32_t>(retained.low64()) &
			0x7f'ffffU;
	} else if (retained.compare(
			   BigUnsigned{1}.shifted_left(23U)) >= 0) {
		exponent_field = 1U;
	} else {
		fraction = static_cast<std::uint32_t>(retained.low64());
	}
	if (flush_output && exponent_field == 0U && fraction != 0U) {
		const std::uint32_t flags = report_flags ?
			fpsr_underflow | fpsr_inexact : 0U;
		return {static_cast<std::uint32_t>(value.negative) << 31U,
			flags};
	}
	std::uint32_t flags{};
	if (report_flags && inexact) {
		flags |= fpsr_inexact;
		if (top_exponent < -126)
			flags |= fpsr_underflow;
	}
	return {
		(static_cast<std::uint32_t>(value.negative) << 31U) |
			(exponent_field << 23U) | fraction,
		flags};
}

[[nodiscard]] constexpr std::uint32_t default_nan(
	std::uint32_t fpcr) noexcept
{
	return (fpcr & fpcr_ah) != 0U ? 0xffc0'0000U : 0x7fc0'0000U;
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(std::uint32_t fpcr, bool extended_dot)
{
	using ::uml::cpu::ArmFeature;
	auto requirements =
		engine_tests::NativeRequirements::features(ArmFeature::bf16);
	if (extended_dot)
		requirements = requirements | ArmFeature::ebf16;
	if ((fpcr & (fpcr_fiz | fpcr_ah)) != 0U)
		requirements = requirements | ArmFeature::afp;
	return requirements;
}

} // namespace arm64_bfloat_arithmetic_test
