// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "program.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace uml::cpu::x86_64::gadgets {

struct NativeTlbEntry;

struct ProgramSelection final {
	BuildStatus status{BuildStatus::illegal_instruction};
	const Program *program{};
	std::uint64_t fault_address{};
	bool cache_hit{};
};

struct ProgramCacheDiagnostics final {
	std::uint64_t translations{};
	std::uint64_t successor_hits{};
	std::uint64_t validation_attempts{};
	std::uint64_t validation_stamp_hits{};
	std::uint64_t validation_hits{};
	std::uint64_t validation_mismatches{};
	std::uint64_t validation_faults{};
	std::uint64_t validation_sequence_races{};
};

struct ProgramCacheSequences final {
	std::uint64_t mapping_changes{};
	std::uint64_t code_changes{};
	bool interrupted{};
};
using ProgramCacheSequenceReader = ProgramCacheSequences (*)(
	void *context) noexcept;

class Executor final {
public:
	Executor();
	~Executor();
	Executor(Executor &&) noexcept;
	Executor &operator=(Executor &&) noexcept;

	Executor(const Executor &) = delete;
	Executor &operator=(const Executor &) = delete;

	void prepare(::uml::memory::GuestAddressSpace &memory,
		std::uint64_t generation = 0,
		std::uint64_t mapping_changes = 0,
		std::uint64_t code_changes = 0,
		void *execution_context = nullptr,
		ProgramCacheSequenceReader sequence_reader = nullptr) noexcept;
	[[nodiscard]] NativeTlbEntry *tlb_entries() noexcept;
	/* True when this executor holds a translation whose source covers the
	 * page. A store to a page without one cannot invalidate any cached block.
	 */
	[[nodiscard]] bool page_holds_cached_code(std::uint64_t address) noexcept;
	[[nodiscard]] void *execution_context() const noexcept;
	[[nodiscard]] std::uint64_t prepared_code_changes() const noexcept;
	[[nodiscard]] std::uint64_t current_code_changes() const noexcept;
	void invalidate_cached_code(
		std::uint64_t address, std::size_t size) noexcept;
	[[nodiscard]] bool accept_code_changes(
		std::uint64_t code_changes) noexcept;
	[[nodiscard]] ProgramSelection select_program(
		::uml::memory::GuestAddressSpace &memory, std::uint64_t address,
		ExecutionMode mode,
		std::size_t maximum_instructions = 64) noexcept;
	void train_indirect(IndirectLink *site, std::uint64_t target,
		ExecutionMode mode) noexcept;
	[[nodiscard]] const ProgramCacheDiagnostics &cache_diagnostics()
		const noexcept;
	[[nodiscard]] std::size_t resident_generations() const noexcept;

private:
	struct Storage;
	std::unique_ptr<Storage> storage_;
};

} // namespace uml::cpu::x86_64::gadgets
