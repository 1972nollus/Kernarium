// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace uml::memory {

/* Which host pages of guest RAM the guest holds.
 *
 * mem= is a ceiling. The guest commits a range before it uses it and releases
 * a range it no longer uses; a released page is dropped from the process
 * footprint and, where guest RAM is wired, unwired. Darwin keeps a released
 * page's contents only until it wants the memory and stops counting it, so a
 * page written again without a commit would be used but not counted - the
 * process could pass its memory limit unseen - and would stay unwired.
 *
 * Offsets index the physical-memory backing, the guest's physical addresses.
 * One bit per host page records the state and only a page whose state changes
 * reaches the host, so repeated, overlapping and concurrent calls are safe.
 * Offsets below `managed_begin` name memory this object does not own, the
 * signed kernel image, and are accepted and ignored. */
class GuestRamCommitment final {
public:
	/* Pages from `uncommitted_begin` start released; pages before it start
	 * committed (and, with `wire`, are expected to be wired already). */
	GuestRamCommitment(std::byte *physical_root, std::size_t size,
		std::size_t managed_begin, std::size_t uncommitted_begin,
		bool wire, std::size_t page_size);
	GuestRamCommitment(const GuestRamCommitment &) = delete;
	GuestRamCommitment &operator=(const GuestRamCommitment &) = delete;

	/* 0 on success, -1 with errno set otherwise. EINVAL for a range that is
	 * not page aligned or leaves the backing. */
	[[nodiscard]] int release(std::uint64_t offset,
		std::uint64_t length) noexcept;
	[[nodiscard]] int commit(std::uint64_t offset,
		std::uint64_t length) noexcept;

	/* Offsets below this belong to the signed kernel image; they are never
	 * released, so whoever maps them keeps them wired. */
	[[nodiscard]] std::size_t managed_begin() const noexcept;
	[[nodiscard]] std::size_t managed_bytes() const noexcept;
	[[nodiscard]] std::size_t released_bytes() const noexcept;
	[[nodiscard]] std::uint64_t wire_failures() const noexcept;

private:
	enum class Change : std::uint8_t { release, commit };

	[[nodiscard]] int change(Change kind, std::uint64_t offset,
		std::uint64_t length) noexcept;
	[[nodiscard]] int apply(Change kind, std::size_t first_page,
		std::size_t pages) noexcept;

	std::byte *root_;
	std::size_t size_;
	std::size_t managed_begin_;
	std::size_t page_size_;
	std::size_t page_count_;
	bool wire_;
	/* Bit set: the page is released (or was never committed). */
	std::unique_ptr<std::atomic<std::uint64_t>[]> released_;
	std::atomic<std::size_t> released_pages_{0};
	std::atomic<std::uint64_t> wire_failures_{0};
};

/* The process's guest RAM, published once the kernel's physical window exists.
 * Null before that. */
[[nodiscard]] GuestRamCommitment *guest_ram_commitment() noexcept;
void publish_guest_ram_commitment(
	std::unique_ptr<GuestRamCommitment> commitment) noexcept;

/* Bridge entry points: 0 on success, -1 with errno. Before guest RAM exists
 * there is nothing to change and they fail with ENXIO. */
[[nodiscard]] int release_guest_ram(std::uint64_t offset,
	std::uint64_t length) noexcept;
[[nodiscard]] int commit_guest_ram(std::uint64_t offset,
	std::uint64_t length) noexcept;

} // namespace uml::memory
