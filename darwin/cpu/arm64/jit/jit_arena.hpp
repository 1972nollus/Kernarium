// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_linking.hpp"
#include "jit_runtime_contract.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::cpu::arm64::jit::detail {

/* The only first-party owner of writable JIT mappings and code publication.
 * Gadget builds do not include or call this interface.
 */
class ExecutableArena final {
public:
	struct StoredBlock final {
		void *code{};
		JitBlockLink *target_link{};
		std::size_t direct_link_refusals{};

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return code != nullptr;
		}
	};

	explicit ExecutableArena(std::size_t size) noexcept;
	~ExecutableArena();

	ExecutableArena(const ExecutableArena &) = delete;
	ExecutableArena &operator=(const ExecutableArena &) = delete;

	[[nodiscard]] bool valid() const noexcept;
	[[nodiscard]] void *store(
		std::span<const std::uint32_t> words) noexcept;
	/* Stores an ordinary cached translation. Direct literals point at the
	 * link table's unique anchor for their address space and guest target;
	 * indirect literals
	 * retain their compiler value for a local translation or use the supplied
	 * executor-private value for a shared translation. Literal patching happens
	 * in the same JIT write window as code publication.
	 */
	[[nodiscard]] StoredBlock store_block(
		std::uint64_t pc,
		std::span<const std::uint32_t> words,
		std::span<const JitRelocation> relocations,
		std::span<const std::uintptr_t> values,
		JitBlockLinkTable &links,
		JitBlockLink &empty_link,
		unsigned long generation = 0) noexcept;
	void activate_link(JitBlockLink &link,
		const JitBlockLinkActivation &activation) noexcept;
	void deactivate_links(
		std::span<JitBlockLink *const> active_targets) noexcept;
	void reset() noexcept;

	[[nodiscard]] static bool available() noexcept;

private:
	void *mapping_{};
	std::size_t used_{};
	std::size_t size_{};
};

} // namespace uml::cpu::arm64::jit::detail
