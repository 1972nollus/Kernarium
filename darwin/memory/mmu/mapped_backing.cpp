// SPDX-License-Identifier: GPL-2.0-or-later
#include "mapped_backing.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>

namespace uml::memory {
namespace {

constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;

} // namespace

struct MappedBacking::TagPage final {
	TagPage() noexcept
	{
		for (auto &tag : tags)
			tag.store(0, std::memory_order_relaxed);
	}

	std::array<std::atomic_uint8_t, page_size / 16> tags;
};

std::shared_ptr<MappedBacking> MappedBacking::create(
	int fd, const struct stat &status)
{
	if (status.st_size <= 0 ||
	    static_cast<std::uintmax_t>(status.st_size) > SIZE_MAX)
		return {};
	const auto size = static_cast<std::size_t>(status.st_size);
	void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED)
		return {};
	return std::shared_ptr<MappedBacking>{
		new MappedBacking{static_cast<std::byte *>(mapping), size}};
}

MappedBacking::MappedBacking(std::byte *data, std::size_t size) noexcept
	: data_(data), size_(size)
{
}

MappedBacking::~MappedBacking()
{
	if (data_ != nullptr)
		(void)munmap(data_, size_);
}

std::byte *MappedBacking::at(std::uint64_t offset) const noexcept
{
	return offset < size_ ? data_ + static_cast<std::size_t>(offset) : nullptr;
}

std::uint8_t MappedBacking::load_allocation_tag(
	std::uint64_t offset) const noexcept
{
	const auto page = offset / page_size;
	const auto index = static_cast<std::size_t>(
		(offset & (page_size - 1)) / 16);
	std::shared_lock lock{tags_mutex_};
	const auto found = tag_pages_.find(page);
	return found == tag_pages_.end() ? 0 :
		found->second->tags[index].load(std::memory_order_acquire);
}

bool MappedBacking::store_allocation_tag(
	std::uint64_t offset, std::uint8_t tag) noexcept
{
	const auto page = offset / page_size;
	const auto index = static_cast<std::size_t>(
		(offset & (page_size - 1)) / 16);
	TagPage *tags = nullptr;
	{
		std::shared_lock lock{tags_mutex_};
		const auto found = tag_pages_.find(page);
		if (found != tag_pages_.end())
			tags = found->second.get();
	}
	if (tags == nullptr) {
		std::lock_guard lock{tags_mutex_};
		auto &slot = tag_pages_[page];
		if (!slot) {
			try {
				slot = std::make_unique<TagPage>();
			} catch (...) {
				return false;
			}
		}
		tags = slot.get();
	}
	tags->tags[index].store(tag & 15U, std::memory_order_release);
	return true;
}

} // namespace uml::memory
