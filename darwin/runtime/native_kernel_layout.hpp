// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace uml::runtime {

struct NativeKernelLayout final {
	std::uintptr_t load_bias{};
	std::uintptr_t window_start{};
	std::uintptr_t window_end{};
	bool valid{};
	/* The host already holds the whole window as one reservation, so the
	 * kernel image and guest RAM must be mapped over it in place. */
	bool reserved{};
	/* With `valid` false: the geometry fits, but the host would not reserve
	 * the window a physical iOS device requires. */
	bool reservation_failed{};
};

template<std::uint64_t LinkAddress, std::uint64_t AddressCeiling,
	std::uint64_t MinimumAuxiliarySize, std::uint64_t MaximumAuxiliarySize,
	std::uint64_t PageSize, std::uint64_t SharedMemoryReservationSize,
	std::uint64_t StubPageCount>
class NativeKernelLayoutGeometry final {
	static_assert(PageSize != 0 && (PageSize & (PageSize - 1U)) == 0);
	static_assert(LinkAddress <= AddressCeiling);
	static_assert(MinimumAuxiliarySize <= MaximumAuxiliarySize);
	static_assert(MaximumAuxiliarySize <= AddressCeiling - LinkAddress);
	static_assert((MinimumAuxiliarySize & (PageSize - 1U)) == 0);
	static_assert((MaximumAuxiliarySize & (PageSize - 1U)) == 0);
	static_assert((SharedMemoryReservationSize & (PageSize - 1U)) == 0);
	static_assert(StubPageCount <=
		std::numeric_limits<std::uint64_t>::max() / PageSize);

	/* The kernel places its stub pages and the guarded vhost-user
	 * shared-memory aperture above the RAM-scaled auxiliary allowance, so
	 * the admitted native window must reserve that span too. Anything less
	 * leaves the kernel's own stub write outside the window the host will
	 * admit.
	 */
	static constexpr std::uint64_t reserved_auxiliary =
		SharedMemoryReservationSize + StubPageCount * PageSize;
	static_assert(SharedMemoryReservationSize <=
		std::numeric_limits<std::uint64_t>::max() -
			StubPageCount * PageSize);
	static_assert(reserved_auxiliary <=
		AddressCeiling - LinkAddress - MaximumAuxiliarySize);

public:
	[[nodiscard]] static constexpr NativeKernelLayout derive(
		std::uint64_t guest_memory) noexcept
	{
		if (guest_memory == 0 ||
		    guest_memory > std::numeric_limits<std::uint64_t>::max() -
			(PageSize - 1U))
			return {};
		const auto rounded = (guest_memory + PageSize - 1U) &
			~(PageSize - 1U);
		/* The auxiliary allowance tracks the page-rounded RAM
		 * one-for-one between its floor and its ceiling, as the kernel
		 * derives it. */
		const auto floored = rounded < MinimumAuxiliarySize ?
			MinimumAuxiliarySize : rounded;
		/* Capped so RAM plus the vmalloc allowance stays inside the
		 * signed iOS envelope; vmalloc's own consumption is bounded by
		 * the page allocator rather than by this reservation. The fixed
		 * kernel reservation is added after the cap because it is not
		 * part of that allowance. */
		const auto capped = floored > MaximumAuxiliarySize ?
			MaximumAuxiliarySize : floored;
		const auto auxiliary = capped + reserved_auxiliary;
		constexpr auto envelope = AddressCeiling - LinkAddress;
		if (rounded == 0 || rounded > envelope ||
		    auxiliary > envelope - rounded)
			return {};
		const auto start = AddressCeiling - auxiliary - rounded;
		return {
			.load_bias = static_cast<std::uintptr_t>(start - LinkAddress),
			.window_start = static_cast<std::uintptr_t>(start),
			.window_end = static_cast<std::uintptr_t>(AddressCeiling),
			.valid = true,
		};
	}

	[[nodiscard]] static constexpr NativeKernelLayout derive_at(
		std::uint64_t guest_memory, std::uint64_t start) noexcept
	{
		const auto packed = derive(guest_memory);
		if (!packed.valid || start < LinkAddress ||
		    start > AddressCeiling)
			return {};
		const auto length = packed.window_end - packed.window_start;
		if (length > AddressCeiling - start)
			return {};
		return {
			.load_bias = static_cast<std::uintptr_t>(start - LinkAddress),
			.window_start = static_cast<std::uintptr_t>(start),
			.window_end = static_cast<std::uintptr_t>(start + length),
			.valid = true,
		};
	}
};

/* The layout for this process. It is invalid when guest_memory does not fit
 * the product's envelope, and also, with `reservation_failed` set, when a
 * physical iOS device cannot reserve its window: guest RAM is mapped over the
 * window with MAP_FIXED, which must never land on a range the host could
 * have handed to someone else. */
[[nodiscard]] NativeKernelLayout prepare_native_kernel_layout(
	std::size_t guest_memory) noexcept;

/* Reserve a window of the geometry's length as one protectionless mapping,
 * at the geometry's address when the host still offers it and wherever the
 * host has room otherwise. The whole window stays reserved: the kernel image,
 * its physical prefix and guest RAM are all mapped over it in place. Returns
 * an invalid layout when no suitable window can be reserved. */
[[nodiscard]] NativeKernelLayout reserve_native_kernel_window(
	const NativeKernelLayout &geometry) noexcept;

class NativeKernelLayoutScope final {
public:
	explicit NativeKernelLayoutScope(
		const NativeKernelLayout &layout) noexcept;
	~NativeKernelLayoutScope();

	NativeKernelLayoutScope(const NativeKernelLayoutScope &) = delete;
	NativeKernelLayoutScope &operator=(
		const NativeKernelLayoutScope &) = delete;

	[[nodiscard]] bool entered() const noexcept { return entered_; }

private:
	bool entered_{};
};

} // namespace uml::runtime
