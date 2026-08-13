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
	std::uint64_t AuxiliarySize, std::uint64_t PageSize>
class NativeKernelLayoutGeometry final {
	static_assert(PageSize != 0 && (PageSize & (PageSize - 1U)) == 0);
	static_assert(LinkAddress <= AddressCeiling);
	static_assert(AuxiliarySize <= AddressCeiling - LinkAddress);

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
		constexpr auto maximum_memory =
			AddressCeiling - LinkAddress - AuxiliarySize;
		if (rounded == 0 || rounded > maximum_memory)
			return {};
		const auto start = AddressCeiling - AuxiliarySize - rounded;
		return {
			.load_bias = static_cast<std::uintptr_t>(start - LinkAddress),
			.window_start = static_cast<std::uintptr_t>(start),
			.window_end = static_cast<std::uintptr_t>(AddressCeiling),
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
