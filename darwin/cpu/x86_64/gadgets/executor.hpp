// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "program.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace uml::cpu::x86_64::gadgets {

struct NativeTlbEntry;
class CodePageRegistry;

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

struct ProgramCodeChange final {
	std::uint64_t address{};
	std::uint64_t length{};
};

/* The guest ranges whose mapping changed after `sequence`, or a negative
 * result when the caller has to retire every software-TLB entry. */
struct ProgramMappingChange final {
	std::uint64_t address{};
	std::uint64_t length{};
};
using MappingChangeReader = int (*)(void *context, std::uint64_t generation,
	std::uint64_t sequence, ProgramMappingChange *changes,
	std::size_t capacity, std::uint64_t *current_sequence) noexcept;

/* The guest ranges whose code changed after `sequence`, or a negative result
 * when the caller has to retire every cached link. */
using ProgramCacheCodeChangeReader = int (*)(void *context,
	std::uint64_t sequence, ProgramCodeChange *changes,
	std::size_t capacity, std::uint64_t *current_sequence) noexcept;

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
		ProgramCacheSequenceReader sequence_reader = nullptr,
		ProgramCacheCodeChangeReader code_change_reader = nullptr,
		MappingChangeReader mapping_change_reader = nullptr) noexcept;
	[[nodiscard]] NativeTlbEntry *tlb_entries() noexcept;
	/* True when this executor holds a translation whose source covers the
	 * page. A store to a page without one cannot invalidate any cached block.
	 */
	[[nodiscard]] bool page_holds_cached_code(std::uint64_t address) noexcept;
	/* The pages holding cached code in any executor of the prepared address
	 * space, or null before the first prepare. */
	[[nodiscard]] CodePageRegistry *code_page_registry() const noexcept;
	/* Bracket every native run so a translating executor can wait for the
	 * stores this one has already translated. */
	void enter_engine() noexcept;
	void leave_engine() noexcept;
	/* The host takes exclusive accesses at any alignment inside a 16-byte
	 * granule (FEAT_LSE2), so an unaligned locked operand that fits in one
	 * keeps the exclusive path. Off until the runtime reports the feature. */
	void permit_unaligned_exclusives(bool value) noexcept;
	[[nodiscard]] bool unaligned_exclusives() const noexcept;
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
	[[nodiscard]] const TargetLinkEntry *target_links(
		ExecutionMode mode) noexcept;
	[[nodiscard]] const ProgramCacheDiagnostics &cache_diagnostics()
		const noexcept;
	[[nodiscard]] std::size_t resident_generations() const noexcept;

private:
	struct Storage;
	std::unique_ptr<Storage> storage_;
};

} // namespace uml::cpu::x86_64::gadgets
