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
namespace {

/* Publication route for the MAP_JIT arena.
 *
 * macOS declares pthread_jit_write_protect_np and calls it directly. No iOS
 * product has a publication route: a physical device cannot obtain the
 * entitlement, so the arena reports itself unavailable there rather than
 * mapping memory it could never publish into.
 *
 * The Simulator is deliberately held to the same answer even though it runs on
 * macOS and could reach the toggle dynamically. Letting it translate through a
 * path production devices cannot use would make the Simulator unrepresentative
 * of the product it exists to stand in for.
 */
#if defined(__APPLE__) && defined(__aarch64__) && defined(MAP_JIT) && \
	!defined(__IPHONE_OS_VERSION_MAX_ALLOWED)
void jit_write_protect(int enable) noexcept
{
	pthread_jit_write_protect_np(enable);
}

constexpr bool jit_publication_supported() noexcept
{
	return true;
}
#else
constexpr void jit_write_protect(int) noexcept {}

constexpr bool jit_publication_supported() noexcept
{
	return false;
}
#endif

} // namespace

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
	jit_write_protect(0);
	std::memcpy(destination, words.data(), bytes);
	jit_write_protect(1);
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
	JitBlockLink &empty_link,
	unsigned long generation) noexcept
{
	if (!values.empty() && relocations.size() != values.size())
		return {};
	const auto bytes = words.size_bytes();
	const auto aligned = (used_ + 15U) & ~std::size_t{15};
	std::array<std::uint64_t, maximum_block_instructions + 1U>
		missing_keys{};
	std::size_t missing_count = 0;
	const auto add_missing = [&](std::uint64_t key) noexcept {
		if (links.find(key, generation) != nullptr)
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
	for (std::size_t index = 0; index < missing_count; ++index)
		(void)links.find_or_create(missing_keys[index], generation);
	auto *destination = static_cast<std::byte *>(mapping_) + aligned;
	jit_write_protect(0);
	std::memcpy(destination, words.data(), bytes);
	auto *stored = reinterpret_cast<std::uint32_t *>(destination);
	auto *target_link = links.find(pc, generation);
	std::size_t direct_refusals = 0;
	for (std::size_t index = 0; index < relocations.size(); ++index) {
		const auto &relocation = relocations[index];
		std::uint64_t value = 0;
		if (relocation.kind == JitRelocationKind::direct_link) {
			auto *record = links.find(relocation.key, generation);
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
	jit_write_protect(1);
	sys_icache_invalidate(destination, bytes);
	used_ = code_end;
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
	link.instruction_count = activation.instruction_count;
	link.internal_entry = activation.internal_entry;
	link.allowed_selectors = activation.allowed_selectors;
	link.activation_epoch = activation.epoch;
}

void ExecutableArena::deactivate_links(
	std::span<JitBlockLink *const> active_targets) noexcept
{
	for (auto *link : active_targets) {
		link->instruction_count = 0;
		link->internal_entry = 0;
		link->allowed_selectors = 0;
		link->activation_epoch = 0;
	}
}

void ExecutableArena::reset() noexcept
{
	used_ = 0;
}

bool ExecutableArena::available() noexcept
{
	static const bool supported = [] {
#if defined(__APPLE__) && defined(__aarch64__) && defined(MAP_JIT)
		/* A successful MAP_JIT mapping is necessary but not sufficient:
		 * without a W^X toggle the first store into it faults. Requiring
		 * the publication route keeps this from promising an engine that
		 * would abort on its first translated block.
		 */
		if (!jit_publication_supported())
			return false;
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
