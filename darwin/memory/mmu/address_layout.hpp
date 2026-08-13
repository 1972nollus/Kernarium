// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <limits>

namespace uml::memory {

enum class AddressDomain : std::uint8_t {
	guest,
	native,
	invalid,
};

template<std::uint64_t GuestLimit, std::uint64_t NativeWindowStart,
	std::uint64_t NativeWindowEnd>
class AddressLayout final {
	static_assert(NativeWindowStart <= NativeWindowEnd);
	static_assert(NativeWindowEnd <= GuestLimit);

	enum class Segment : std::uint8_t {
		guest_low,
		native_window,
		guest_high,
		native_high,
	};

	[[nodiscard]] static constexpr Segment segment(
		const std::uint64_t address) noexcept
	{
		if (address < NativeWindowStart)
			return Segment::guest_low;
		if (address < NativeWindowEnd)
			return Segment::native_window;
		if (address < GuestLimit)
			return Segment::guest_high;
		return Segment::native_high;
	}

public:
	[[nodiscard]] static constexpr AddressDomain classify(
		const std::uint64_t address, const std::uint64_t length) noexcept
	{
		if (length > std::numeric_limits<std::uint64_t>::max() - address)
			return AddressDomain::invalid;
		const auto last = length == 0 ? address : address + length - 1;
		const auto first_segment = segment(address);
		if (first_segment != segment(last))
			return AddressDomain::invalid;
		return first_segment == Segment::guest_low ||
			first_segment == Segment::guest_high ?
			AddressDomain::guest : AddressDomain::native;
	}

	[[nodiscard]] static constexpr bool is_guest(
		const std::uint64_t address) noexcept
	{
		return classify(address, 1) == AddressDomain::guest;
	}

	[[nodiscard]] static constexpr bool is_native_window(
		const std::uint64_t address, const std::uint64_t length) noexcept
	{
		if (length > std::numeric_limits<std::uint64_t>::max() - address)
			return false;
		const auto last = length == 0 ? address : address + length - 1;
		return segment(address) == Segment::native_window &&
			segment(last) == Segment::native_window;
	}
};

class ConfiguredAddressLayout final {
public:
	[[nodiscard]] static AddressDomain classify(
		std::uint64_t address, std::uint64_t length) noexcept;
	[[nodiscard]] static bool is_guest(std::uint64_t address) noexcept;
	[[nodiscard]] static bool is_native_window(
		std::uint64_t address, std::uint64_t length) noexcept;
};

/* Embedded-kernel admission serializes process-wide native mappings. The
 * runtime narrows the maximum iOS envelope to the exact RAM-plus-auxiliary
 * window before entering the relocated kernel, then restores the envelope
 * after the run. Static macOS products compile these operations to an
 * exact-value check and retain the previous constant classifier.
 */
[[nodiscard]] bool configure_native_address_window(
	std::uint64_t start, std::uint64_t end) noexcept;
void reset_native_address_window() noexcept;

} // namespace uml::memory
