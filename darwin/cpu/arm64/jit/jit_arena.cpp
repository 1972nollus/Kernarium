// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_arena.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <sys/mman.h>

#include <libkern/OSCacheControl.h>
#include <pthread.h>

namespace uml::cpu::arm64::jit::detail {

ExecutableArena::ExecutableArena(std::size_t size) noexcept : size_{size}
{
	mapping_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
	if (mapping_ == MAP_FAILED)
		mapping_ = nullptr;
}

ExecutableArena::~ExecutableArena()
{
	if (mapping_ != nullptr)
		(void)munmap(mapping_, size_);
}

bool ExecutableArena::valid() const noexcept
{
	return mapping_ != nullptr;
}

void *ExecutableArena::store(
	std::span<const std::uint32_t> words) noexcept
{
	const auto bytes = words.size_bytes();
	const auto aligned = (used_ + 15U) & ~std::size_t{15};
	if (mapping_ == nullptr || bytes == 0 || aligned > size_ ||
	    bytes > size_ - aligned)
		return nullptr;
	auto *destination = static_cast<std::byte *>(mapping_) + aligned;
#if !defined(__IPHONE_OS_VERSION_MAX_ALLOWED)
	pthread_jit_write_protect_np(0);
#endif
	std::memcpy(destination, words.data(), bytes);
#if !defined(__IPHONE_OS_VERSION_MAX_ALLOWED)
	pthread_jit_write_protect_np(1);
#endif
	sys_icache_invalidate(destination, bytes);
	used_ = aligned + bytes;
	return destination;
}

ExecutableArena::StoredBlock ExecutableArena::store_block(
	std::uint64_t pc,
	std::span<const std::uint32_t> words,
	std::span<const JitRelocation> relocations,
	std::span<const std::uintptr_t> values,
	JitBlockLinkTable &links,
	JitBlockLink &empty_link) noexcept
{
	if (!values.empty() && relocations.size() != values.size())
		return {};
	const auto bytes = words.size_bytes();
	const auto aligned = (used_ + 15U) & ~std::size_t{15};
	std::array<std::uint64_t, maximum_block_instructions + 1U>
		missing_keys{};
	std::size_t missing_count = 0;
	const auto add_missing = [&](std::uint64_t key) noexcept {
		if (links.find(key) != nullptr)
			return true;
		for (std::size_t index = 0; index < missing_count; ++index)
			if (missing_keys[index] == key)
				return true;
		if (missing_count == missing_keys.size())
			return false;
		missing_keys[missing_count++] = key;
		return true;
	};
	if (!add_missing(pc))
		return {};
	for (const auto &relocation : relocations) {
		if (relocation.word + 1U >= words.size())
			return {};
		switch (relocation.kind) {
		case JitRelocationKind::direct_link:
			if (!add_missing(relocation.key))
				return {};
			break;
		case JitRelocationKind::indirect_link:
			break;
		default:
			return {};
		}
	}
	if (mapping_ == nullptr || bytes == 0 || aligned > size_ ||
	    bytes > size_ - aligned)
		return {};
	const auto code_end = aligned + bytes;
	if (code_end > std::numeric_limits<std::size_t>::max() - 63U)
		return {};
	const auto link_offset = (code_end + 63U) & ~std::size_t{63};
	const auto remaining_links = links.capacity() - links.size();
	const auto link_count = std::min(missing_count, remaining_links);
	if (link_count != 0 && (link_offset > size_ ||
	    link_count > (size_ - link_offset) / sizeof(JitBlockLink)))
		return {};
	const auto allocation_end = link_count == 0 ? code_end :
		link_offset + link_count * sizeof(JitBlockLink);
	auto *destination = static_cast<std::byte *>(mapping_) + aligned;
	auto *records = reinterpret_cast<JitBlockLink *>(
		static_cast<std::byte *>(mapping_) + link_offset);
#if !defined(__IPHONE_OS_VERSION_MAX_ALLOWED)
	pthread_jit_write_protect_np(0);
#endif
	std::memcpy(destination, words.data(), bytes);
	auto *stored = reinterpret_cast<std::uint32_t *>(destination);
	for (std::size_t index = 0; index < link_count; ++index) {
		auto *record = std::construct_at(
			records + index,
			JitBlockLink{.guest_pc = missing_keys[index]});
		const auto insertion = links.insert(*record);
		if (insertion.inserted)
			record->next_target = insertion.previous;
	}
	auto *target_link = links.find(pc);
	std::size_t direct_refusals = 0;
	for (std::size_t index = 0; index < relocations.size(); ++index) {
		const auto &relocation = relocations[index];
		std::uint64_t value = 0;
		if (relocation.kind == JitRelocationKind::direct_link) {
			auto *record = links.find(relocation.key);
			if (record == nullptr) {
				++direct_refusals;
				record = &empty_link;
			}
			value = reinterpret_cast<std::uintptr_t>(record);
		} else if (values.empty()) {
			continue;
		} else {
			value = values[index];
		}
		stored[relocation.word] =
			static_cast<std::uint32_t>(value);
		stored[relocation.word + 1U] =
			static_cast<std::uint32_t>(value >> 32U);
	}
#if !defined(__IPHONE_OS_VERSION_MAX_ALLOWED)
	pthread_jit_write_protect_np(1);
#endif
	sys_icache_invalidate(destination, bytes);
	used_ = allocation_end;
	return {
		.code = destination,
		.target_link = target_link,
		.direct_link_refusals = direct_refusals,
	};
}

void ExecutableArena::activate_link(
	JitBlockLink &link,
	const JitBlockLinkActivation &activation) noexcept
{
#if !defined(__IPHONE_OS_VERSION_MAX_ALLOWED)
	pthread_jit_write_protect_np(0);
#endif
	link.instruction_count = activation.instruction_count;
	link.internal_entry = activation.internal_entry;
	link.allowed_selectors = activation.allowed_selectors;
	link.activation_epoch = activation.epoch;
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
	link.profile_read_before_write = activation.profile_read_before_write;
	link.profile_mask_valid = activation.profile_mask_valid;
#endif
#if !defined(__IPHONE_OS_VERSION_MAX_ALLOWED)
	pthread_jit_write_protect_np(1);
#endif
}

void ExecutableArena::deactivate_links(
	std::span<JitBlockLink *const> active_targets) noexcept
{
	if (active_targets.empty())
		return;
#if !defined(__IPHONE_OS_VERSION_MAX_ALLOWED)
	pthread_jit_write_protect_np(0);
#endif
	for (auto *link : active_targets) {
			link->instruction_count = 0;
			link->internal_entry = 0;
			link->allowed_selectors = 0;
			link->activation_epoch = 0;
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
			link->profile_read_before_write = 0;
			link->profile_mask_valid = false;
#endif
	}
#if !defined(__IPHONE_OS_VERSION_MAX_ALLOWED)
	pthread_jit_write_protect_np(1);
#endif
}

void ExecutableArena::reset() noexcept
{
	used_ = 0;
}

bool ExecutableArena::available() noexcept
{
	static const bool supported = [] {
#if defined(__APPLE__) && defined(__aarch64__) && defined(MAP_JIT)
		void *mapping = mmap(nullptr, 16U * 1024U,
			PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
		if (mapping == MAP_FAILED)
			return false;
		(void)munmap(mapping, 16U * 1024U);
		return true;
#else
		return false;
#endif
	}();
	return supported;
}

} // namespace uml::cpu::arm64::jit::detail
