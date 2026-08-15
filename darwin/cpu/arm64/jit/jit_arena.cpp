// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_arena.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <memory>
#include <sys/mman.h>

#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>

namespace uml::cpu::arm64::jit::detail {
namespace {

/* Publication route for the MAP_JIT arena.
 *
 * macOS declares pthread_jit_write_protect_np, so it calls the toggle
 * directly. The iOS SDK marks the symbol unavailable for every iOS target,
 * which previously compiled the toggle out of the Simulator too -- yet the
 * Simulator runs on macOS and does enforce MAP_JIT W^X, so a store without the
 * toggle faults. The symbol is present in the Simulator runtime and reachable
 * dynamically, which is the only difference this needs. A physical device has
 * no route at all without a JIT entitlement, so it keeps the no-op and reports
 * the arena unavailable rather than mapping memory it can never publish into.
 */
#if defined(__APPLE__) && defined(__aarch64__) && defined(MAP_JIT)
#if TARGET_OS_SIMULATOR
using JitWriteProtect = void (*)(int);

const JitWriteProtect simulator_write_protect =
	reinterpret_cast<JitWriteProtect>(
		dlsym(RTLD_DEFAULT, "pthread_jit_write_protect_np"));

void jit_write_protect(int enable) noexcept
{
	if (simulator_write_protect != nullptr)
		simulator_write_protect(enable);
}

bool jit_publication_supported() noexcept
{
	return simulator_write_protect != nullptr;
}
#elif !defined(__IPHONE_OS_VERSION_MAX_ALLOWED)
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
	jit_write_protect(0);
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
	jit_write_protect(1);
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
	jit_write_protect(0);
	link.instruction_count = activation.instruction_count;
	link.internal_entry = activation.internal_entry;
	link.allowed_selectors = activation.allowed_selectors;
	link.activation_epoch = activation.epoch;
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
	link.profile_read_before_write = activation.profile_read_before_write;
	link.profile_mask_valid = activation.profile_mask_valid;
#endif
	jit_write_protect(1);
}

void ExecutableArena::deactivate_links(
	std::span<JitBlockLink *const> active_targets) noexcept
{
	if (active_targets.empty())
		return;
	jit_write_protect(0);
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
	jit_write_protect(1);
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
