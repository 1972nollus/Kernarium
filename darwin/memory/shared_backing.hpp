// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

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

/* Guest RAM when (device, inode) identifies the anonymous backing. A caller
 * that recorded a descriptor's identity when it mapped it reaches the RAM
 * without an fstat per access. memory is empty until the kernel's physical
 * window exists. */
struct AnonymousBackingMemory final {
	bool anonymous{};
	std::span<std::byte> memory;
};

[[nodiscard]] AnonymousBackingMemory anonymous_shared_backing_memory(
	std::uint64_t device, std::uint64_t inode) noexcept;

/* Acquire a read/write consumer view without creating another Mach mapping
 * when fd identifies the anonymous guest-RAM root. The kernel's in-place
 * physical window must have been established first. Other protections and
 * ordinary descriptors retain mmap/vm_remap ownership semantics. */
[[nodiscard]] SharedBackingView acquire_shared_backing_view(int fd,
	std::uint64_t offset, std::size_t length, int protection) noexcept;

[[nodiscard]] void *map_shared_backing(void *address, std::size_t length,
	int protection, int flags, int fd, std::uint64_t offset) noexcept;

/* Remove a mapping inside a reserved guest window without giving the address
 * range back to the process.
 *
 * The window is one PROT_NONE MAP_NORESERVE reservation, and guest pages are
 * mapped over it with MAP_FIXED. munmap would delete the reservation with the
 * mapping and leave a genuine free gap that any unrelated mmap(NULL) or
 * mach_vm_allocate(ANYWHERE) in this process may be given - and Darwin's
 * allocation hint follows the most recently freed range, so it is not merely
 * possible but preferred. A single MAP_FIXED PROT_NONE anonymous mapping
 * replaces the range atomically, costs the same one syscall, commits no pages,
 * and keeps the window fully reserved at every instant.
 *
 * Returns 0 on success and -1 with errno set otherwise; the range is left
 * mapped, not released, on failure. */
[[nodiscard]] int reserve_host_range(
	void *address, std::size_t length) noexcept;

/* Translate a Mach failure into the errno the kernel reads. An exhausted host
 * must arrive as ENOMEM, which is the only value that makes the kernel say the
 * host ran out rather than retry an address it believes is merely bad. */
[[nodiscard]] int mach_failure_errno(int result) noexcept;

/* Report a host mapping call that failed. A failure leaves the kernel's page
 * tables describing memory the host does not have, and nothing else in this
 * process says so; the first few are reported unconditionally so the evidence
 * reaches the run's own console. */
void report_mapping_failure(const char *what, const void *address,
	std::size_t length, int error) noexcept;

} // namespace uml::memory
