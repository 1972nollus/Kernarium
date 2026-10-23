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

/* A process registers one anonymous guest-RAM token and never releases it. */
[[nodiscard]] bool register_anonymous_shared_backing(int fd) noexcept;
[[nodiscard]] bool anonymous_shared_backing_registered() noexcept;
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

/* Keep a retiring domain's guest RAM page mapped while a view borrows it.
 *
 * A view of the anonymous root borrows the root's own mapping, and its holder
 * may dereference it until the view is released. In-process backends hold
 * such views for as long as they serve a connection, which can outlast the
 * run whose domain owned the pages: a host stop leaves their queues started,
 * and they are stopped only after the run has returned. When the range lies in
 * the root and any borrowed view is live, the range stays mapped and is handed
 * back to the window's reservation when the last borrowed view is released;
 * the call then returns true and the caller must not release the range
 * itself. A later mapping over the range supersedes the deferred release.
 * Otherwise it returns false and the caller releases the range as usual. */
[[nodiscard]] bool retain_borrowed_guest_ram(
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

/* mlock(2) with the calling thread's asynchronous signals held for the call.
 *
 * XNU wires a range by faulting each page in, and a fault that has to wait,
 * for a page being read in or one another fault holds, waits abortably: a
 * signal posted to the thread meanwhile ends the wire, and mlock unwires what
 * it had wired and fails with ENOMEM. Guest CPU threads take the IPI wake
 * signal while they run kernel code, which is where they commit guest RAM, so
 * a commit that waited for memory, as commits do right after a resume, lost
 * its wiring to an IPI. A signal posted during the call is delivered when it
 * returns. 0 on success, -1 with errno set. */
[[nodiscard]] int wire_host_range(void *address, std::size_t length) noexcept;

} // namespace uml::memory
