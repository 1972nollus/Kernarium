// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_runtime_contract.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::cpu::arm64::jit::detail {

/* The only first-party owner of writable JIT mappings and code publication.
 * Interpreter and gadget builds do not include or call this interface.
 */
class ExecutableArena final {
public:
	explicit ExecutableArena(std::size_t size) noexcept;
	~ExecutableArena();

	ExecutableArena(const ExecutableArena &) = delete;
	ExecutableArena &operator=(const ExecutableArena &) = delete;

	[[nodiscard]] bool valid() const noexcept;
	[[nodiscard]] void *store(
		std::span<const std::uint32_t> words) noexcept;
	/* Stores a translation compiled by another executor, rewriting the
	 * recorded literal slots to this executor's link pointers before the
	 * instruction cache is published. Patching here rather than in a caller
	 * owned buffer keeps every write to executable memory in this file and
	 * saves copying the translation twice.
	 */
	[[nodiscard]] void *store_relocated(
		std::span<const std::uint32_t> words,
		std::span<const JitRelocation> relocations,
		std::span<const std::uintptr_t> values) noexcept;
	void reset() noexcept;

	[[nodiscard]] static bool available() noexcept;

private:
	void *mapping_{};
	std::size_t used_{};
	std::size_t size_{};
};

} // namespace uml::cpu::arm64::jit::detail
