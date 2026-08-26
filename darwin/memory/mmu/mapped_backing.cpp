// SPDX-License-Identifier: GPL-2.0-or-later
#include "mapped_backing.hpp"

#include "allocation_fault.hpp"
#include "../shared_backing.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <utility>

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
	auto mapping = acquire_shared_backing_view(
		fd, 0, size, PROT_READ | PROT_WRITE);
	if (!mapping)
		return {};
	UML_MEMORY_BACKING_MAPPING_CREATED();
	struct CounterGuard final {
		bool owned{true};
		~CounterGuard()
		{
			if (owned)
				UML_MEMORY_BACKING_MAPPING_DESTROYED();
		}
	} guard;
	UML_MEMORY_ALLOCATION_CHECKPOINT(
		testing::AllocationSite::backing);
	auto *backing = new MappedBacking{std::move(mapping), size};
	guard.owned = false;
	return std::shared_ptr<MappedBacking>{backing};
}

MappedBacking::MappedBacking(
	SharedBackingView mapping, std::size_t size) noexcept
	: mapping_(std::move(mapping)), size_(size)
{
}

MappedBacking::~MappedBacking()
{
	if (mapping_)
		UML_MEMORY_BACKING_MAPPING_DESTROYED();
}

std::byte *MappedBacking::at(std::uint64_t offset) const noexcept
{
	auto *data = static_cast<std::byte *>(mapping_.data());
	return offset < size_ ? data + static_cast<std::size_t>(offset) : nullptr;
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
		try {
			std::lock_guard lock{tags_mutex_};
			auto &slot = tag_pages_[page];
			if (!slot)
				slot = std::make_unique<TagPage>();
			tags = slot.get();
		} catch (...) {
			return false;
		}
	}
	tags->tags[index].store(tag & 15U, std::memory_order_release);
	return true;
}

} // namespace uml::memory
