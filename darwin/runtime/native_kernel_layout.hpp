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
};

template<std::uint64_t LinkAddress, std::uint64_t AddressCeiling,
	std::uint64_t AuxiliaryDivisor, std::uint64_t MinimumAuxiliarySize,
	std::uint64_t MaximumAuxiliarySize, std::uint64_t PageSize>
class NativeKernelLayoutGeometry final {
	static_assert(PageSize != 0 && (PageSize & (PageSize - 1U)) == 0);
	static_assert(AuxiliaryDivisor != 0);
	static_assert(LinkAddress <= AddressCeiling);
	static_assert(MinimumAuxiliarySize <= MaximumAuxiliarySize);
	static_assert(MaximumAuxiliarySize <= AddressCeiling - LinkAddress);
	static_assert((MinimumAuxiliarySize & (PageSize - 1U)) == 0);
	static_assert((MaximumAuxiliarySize & (PageSize - 1U)) == 0);

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
		const auto proportional = rounded / AuxiliaryDivisor +
			(rounded % AuxiliaryDivisor != 0 ? 1U : 0U);
		if (proportional > std::numeric_limits<std::uint64_t>::max() -
			(PageSize - 1U))
			return {};
		const auto scaled_auxiliary =
			(proportional + PageSize - 1U) & ~(PageSize - 1U);
		const auto floored = scaled_auxiliary < MinimumAuxiliarySize ?
			MinimumAuxiliarySize : scaled_auxiliary;
		/* Capped so RAM plus auxiliary stays inside the signed iOS
		 * envelope; vmalloc's own consumption is bounded by the page
		 * allocator rather than by this reservation. */
		const auto auxiliary = floored > MaximumAuxiliarySize ?
			MaximumAuxiliarySize : floored;
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

[[nodiscard]] NativeKernelLayout prepare_native_kernel_layout(
	std::size_t guest_memory) noexcept;

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
