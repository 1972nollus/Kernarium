// SPDX-License-Identifier: GPL-2.0-or-later
#include "executor.hpp"

#include "native_entry.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace uml::cpu::x86_64::gadgets {
namespace {

static_assert(DARWIN_UML_GUEST_PAGE_SIZE == 4096,
	"x86-64 gadgets are a 4 KiB guest product");
inline constexpr std::size_t tlb_entry_count = 1U << 13U;

} // namespace

struct Executor::Storage final {
	std::array<NativeTlbEntry, tlb_entry_count> entries{};
	::uml::memory::GuestAddressSpace *memory{};
	std::uint64_t generation{};
	std::uint64_t changes{};
	bool prepared{};
};

Executor::Executor() : storage_{std::make_unique<Storage>()} {}
Executor::~Executor() = default;
Executor::Executor(Executor &&) noexcept = default;
Executor &Executor::operator=(Executor &&) noexcept = default;

void Executor::prepare(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t generation, std::uint64_t changes) noexcept
{
	if (storage_->prepared && storage_->memory == &memory &&
		storage_->generation == generation && storage_->changes == changes)
		return;
	std::ranges::fill(storage_->entries, NativeTlbEntry{});
	storage_->memory = &memory;
	storage_->generation = generation;
	storage_->changes = changes;
	storage_->prepared = true;
}

NativeTlbEntry *Executor::tlb_entries() noexcept
{
	return storage_->entries.data();
}

} // namespace uml::cpu::x86_64::gadgets
