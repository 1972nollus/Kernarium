// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/decoder.hpp"
#include "memory/guest_address_space.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace uml::cpu::x86_64::gadgets {

struct Gadget;
class Executor;

/* A gadget is immutable normalized instruction data. Translation resolves it
 * exclusively to precompiled arm64 assembly entries; no guest instruction is
 * copied into executable memory and no semantic C++ executor remains.
 */
struct Gadget final {
	std::uint64_t address{};
	Instruction instruction{};
};

/* Cached direct branches name stable, data-only link records. A reset link
 * enters its private fallback stream; an active link enters the cached target
 * without modifying executable memory. The target bounds preserve x86's
 * self-modifying-code check while a native chain crosses program objects.
 */
struct BlockLink final {
	const std::uintptr_t *stream{};
	std::size_t instruction_count{};
	std::uint64_t guest_rip{};
	std::uint64_t program_begin{};
	std::uint64_t program_end{};
};
static_assert(offsetof(BlockLink, stream) == 0);
static_assert(offsetof(BlockLink, instruction_count) == 8);
static_assert(offsetof(BlockLink, guest_rip) == 16);
static_assert(offsetof(BlockLink, program_begin) == 24);
static_assert(offsetof(BlockLink, program_end) == 32);
static_assert(sizeof(BlockLink) == 40);

struct IndirectLinkEntry final {
	std::uint64_t guest_rip{};
	const BlockLink *link{};
};
static_assert(offsetof(IndirectLinkEntry, guest_rip) == 0);
static_assert(offsetof(IndirectLinkEntry, link) == 8);
static_assert(sizeof(IndirectLinkEntry) == 16);

/* Match arm64's monomorphic/bimorphic indirect tier. The translated hit path
 * reads only the two target/link pairs; replacement and live-list state stay
 * in data and never require executable publication. */
struct IndirectLink final {
	std::array<IndirectLinkEntry, 2> entries{};
	std::uint8_t next_replacement{};
	bool listed{};
};
static_assert(offsetof(IndirectLink, entries) == 0);
static_assert(offsetof(IndirectLink, next_replacement) == 32);
static_assert(offsetof(IndirectLink, listed) == 33);
static_assert(sizeof(IndirectLink) == 40);

/* A return site belongs to the call that reached it, not to the RET that
 * leaves it, so a function called from three places overruns a two-way tier
 * keyed on the branch. One direct-mapped tier keyed on the target instead is
 * shared by every indirect branch in an address space: whatever block ran
 * most recently at that guest RIP is reachable from all of them.
 *
 * An entry is only a prediction. The follower compares the guest RIP it has
 * already computed against the recorded one and enters the link only on a
 * match, so a stale or evicted entry costs a fallback and never a wrong
 * branch. Unused entries name an address no block starts at and carry that
 * address's own record, which keeps `link` dereferenceable without a null
 * test on the translated path.
 */
struct TargetLinkEntry final {
	std::uint64_t guest_rip{};
	const BlockLink *link{};
};
static_assert(offsetof(TargetLinkEntry, guest_rip) == 0);
static_assert(offsetof(TargetLinkEntry, link) == 8);
static_assert(sizeof(TargetLinkEntry) == 16);

inline constexpr std::size_t target_link_entries = 4096;
inline constexpr unsigned target_link_fold = 12;
static_assert((target_link_entries & (target_link_entries - 1U)) == 0,
	"the translated tier masks the guest RIP fold");

/* Block starts cluster inside a page, so fold the page number back over the
 * offset before masking. */
[[nodiscard]] inline constexpr std::size_t target_link_index(
	std::uint64_t guest_rip) noexcept
{
	return static_cast<std::size_t>(
		(guest_rip ^ (guest_rip >> target_link_fold)) &
		(target_link_entries - 1U));
}

using TargetLinkTable = std::array<TargetLinkEntry, target_link_entries>;

struct ProgramLinkResolver final {
	void *context{};
	const BlockLink *(*resolve)(void *, std::uint64_t, ExecutionMode){};
	IndirectLink *(*resolve_indirect)(
		void *, std::uint64_t, ExecutionMode){};
};

/* A translation owns two allocations, each at exactly the size the block
 * turned out to need: the native stream the engine runs, and the cold data
 * behind it -- the decoded instructions a budget-limited replay rebuilds from,
 * followed by the guest bytes the decoder read. Keeping the streams in their
 * own allocations is what keeps them near each other in the allocator, which
 * the workloads with the widest code notice. */
class Program final {
public:
	Program() = default;
	Program(ExecutionMode mode, std::uint64_t address, std::size_t byte_count,
		std::span<const Gadget> gadgets,
		std::span<const std::byte> source = {},
		ProgramLinkResolver links = {});

	[[nodiscard]] ExecutionMode execution_mode() const noexcept { return mode_; }
	[[nodiscard]] std::uint64_t address() const noexcept { return address_; }
	[[nodiscard]] std::size_t byte_count() const noexcept { return byte_count_; }
	[[nodiscard]] std::span<const Gadget> stream() const noexcept
	{
		return {reinterpret_cast<const Gadget *>(decoded_.get()), gadgets_};
	}
	[[nodiscard]] std::span<const std::uintptr_t> native_stream() const noexcept
	{
		return {reinterpret_cast<const std::uintptr_t *>(words_storage_.get()),
			words_};
	}
	[[nodiscard]] std::size_t instruction_count() const noexcept
	{
		return gadgets_;
	}
	/* The guest bytes the decoder read for this block, or empty when the
	 * caller asked for none. A cached translation and the source that stands
	 * for it are then one object as well as one observation. */
	[[nodiscard]] std::span<const std::byte> source() const noexcept
	{
		return {decoded_.get() + gadgets_ * sizeof(Gadget), source_};
	}

private:
	ExecutionMode mode_{ExecutionMode::long64};
	std::uint64_t address_{};
	std::size_t byte_count_{};
	/* The words the engine runs, alone. A block's stream is a few hundred
	 * bytes and its decoded instructions a few thousand, so holding both in
	 * one allocation put every hot stream a cold kilobyte or two away from
	 * the next and cost the workloads with the widest code. */
	std::unique_ptr<std::byte[]> words_storage_;
	/* The instructions a budget-limited replay rebuilds from, then the guest
	 * bytes the decoder read. Neither is touched while the block runs. */
	std::unique_ptr<std::byte[]> decoded_;
	std::uint32_t words_{};
	std::uint32_t gadgets_{};
	std::uint32_t source_{};
};

enum class BuildStatus : std::uint8_t {
	built,
	fetch_fault,
	illegal_instruction,
	allocation_failure,
};

struct BuildResult final {
	BuildStatus status{BuildStatus::illegal_instruction};
	Program program{};
	std::uint64_t fault_address{};
	/* The program carries every byte of every instruction this block kept, so
	 * `program.source()` is what the decoder read. A build given no room, or
	 * one whose block outgrew it, clears this and the block cannot be
	 * cached. */
	bool source_recorded{};
};

[[nodiscard]] BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, std::size_t maximum_instructions = 64) noexcept;
[[nodiscard]] BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, ExecutionMode mode,
	std::size_t maximum_instructions = 64) noexcept;
/* `source`, when it is not empty, is scratch the build fills with the bytes the
 * decoder read for the instructions this block keeps, in address order, and the
 * program it returns carries a copy of exactly those bytes. Saving those rather
 * than reading the block again is what makes a cached program and its saved
 * source one observation of guest memory instead of two. */
[[nodiscard]] BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, ExecutionMode mode,
	std::size_t maximum_instructions, ProgramLinkResolver links,
	std::span<std::byte> source = {}) noexcept;

struct ExecuteResult final {
	::uml::cpu::EngineEvent event{::uml::cpu::EngineEvent::fatal};
	std::size_t retired_instructions{};
	::uml::cpu::FaultInfo fault{};
	bool instruction_budget_exhausted{};
};

[[nodiscard]] ExecuteResult execute(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt = {}) noexcept;
[[nodiscard]] ExecuteResult execute(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, Executor &executor,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt = {}) noexcept;

} // namespace uml::cpu::x86_64::gadgets
