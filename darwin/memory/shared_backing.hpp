// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace uml::memory {

class SharedBackingView final {
public:
	SharedBackingView() noexcept = default;
	SharedBackingView(const SharedBackingView &) = delete;
	SharedBackingView &operator=(const SharedBackingView &) = delete;
	SharedBackingView(SharedBackingView &&other) noexcept;
	SharedBackingView &operator=(SharedBackingView &&other) noexcept;
	~SharedBackingView();

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return data_ != nullptr;
	}
	[[nodiscard]] void *data() const noexcept { return data_; }
	[[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
	friend SharedBackingView acquire_shared_backing_view(int,
		std::uint64_t, std::size_t, int) noexcept;

	SharedBackingView(void *data, std::size_t size, bool owns_mapping) noexcept;
	void reset() noexcept;

	void *data_{};
	std::size_t size_{};
	bool owns_mapping_{};
};

[[nodiscard]] bool register_anonymous_shared_backing(int fd) noexcept;
[[nodiscard]] bool is_anonymous_shared_backing(int fd) noexcept;

/* Acquire a read/write consumer view without creating another Mach mapping
 * when fd identifies the anonymous guest-RAM root. Other protections and
 * ordinary descriptors retain mmap/vm_remap ownership semantics. */
[[nodiscard]] SharedBackingView acquire_shared_backing_view(int fd,
	std::uint64_t offset, std::size_t length, int protection) noexcept;

[[nodiscard]] void *map_shared_backing(void *address, std::size_t length,
	int protection, int flags, int fd, std::uint64_t offset) noexcept;

} // namespace uml::memory
