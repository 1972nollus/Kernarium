// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../shared_backing.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

struct stat;

namespace uml::memory {

class MappedBacking final {
public:
	static std::shared_ptr<MappedBacking> create(int fd,
		const struct stat &status);

	MappedBacking(const MappedBacking &) = delete;
	MappedBacking &operator=(const MappedBacking &) = delete;
	~MappedBacking();

	[[nodiscard]] std::byte *at(std::uint64_t offset) const noexcept;
	[[nodiscard]] std::size_t size() const noexcept { return size_; }
	[[nodiscard]] std::uint8_t load_allocation_tag(
		std::uint64_t offset) const noexcept;
	[[nodiscard]] bool store_allocation_tag(
		std::uint64_t offset, std::uint8_t tag) noexcept;
	/* Code-page records are shared by every virtual alias of one physical
	 * backing page. Their addresses remain stable for the backing lifetime so
	 * the static-memory TLB can cache the opaque handle between mapping changes.
	 */
	[[nodiscard]] void *code_page_handle(std::uint64_t offset) noexcept;
	[[nodiscard]] static bool load_code_page_stamp(const void *handle,
		std::uint64_t &identity, std::uint64_t &version) noexcept;
	static void invalidate_code_page(void *handle) noexcept;

private:
	struct TagPage;
	struct CodePage;
	MappedBacking(SharedBackingView mapping, std::size_t size) noexcept;

	SharedBackingView mapping_;
	std::size_t size_;
	mutable std::shared_mutex tags_mutex_;
	std::unordered_map<std::uint64_t, std::unique_ptr<TagPage>> tag_pages_;
	mutable std::shared_mutex code_pages_mutex_;
	std::unordered_map<std::uint64_t, std::unique_ptr<CodePage>> code_pages_;
};

} // namespace uml::memory
