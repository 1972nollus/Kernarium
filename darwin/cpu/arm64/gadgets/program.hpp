// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../cpu_state.hpp"
#include "../../engine.hpp"
#include "../fp_environment.hpp"
#include "tlb.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace uml::cpu::arm64::gadgets {

/* One contiguous stretch of guest instructions a program translated.
 *
 * A block is a single run unless a call was inlined into it, when it becomes
 * the caller's run, the callee's, and the continuation after the return. The
 * run list is what lets a guest PC still be converted to a position in the
 * block once the addresses stop being contiguous, which both the retirement
 * corrections and the stale-program check depend on.
 */
struct GuestRun final {
	std::uint64_t address{};
	std::uint32_t instructions{};
};

/* Most translated streams are small, but a pmr::vector gives every one its
 * own pool allocation. Keep the common prefix in the Program itself and use
 * the same allocator, with the builder's former 34-word reservation, only
 * when a stream spills. */
class ProgramWords final {
public:
	static constexpr std::size_t inline_capacity = 16;

	explicit ProgramWords(std::pmr::memory_resource *resource) noexcept
		: resource_{resource}
	{}

	explicit ProgramWords(std::pmr::vector<std::uintptr_t> words)
		: resource_{words.get_allocator().resource()}
	{
		reserve(words.size());
		std::copy(words.begin(), words.end(), data());
		size_ = words.size();
	}

	ProgramWords(const ProgramWords &other)
		: resource_{std::pmr::get_default_resource()}
	{
		reserve(other.size_);
		std::copy_n(other.data(), other.size_, data());
		size_ = other.size_;
	}

	ProgramWords &operator=(const ProgramWords &other)
	{
		if (this == &other)
			return *this;
		reserve(other.size_);
		std::copy_n(other.data(), other.size_, data());
		size_ = other.size_;
		return *this;
	}

	ProgramWords(ProgramWords &&other) noexcept
		: resource_{other.resource_}, size_{other.size_},
		  capacity_{other.capacity_}, heap_{other.heap_}
	{
		if (heap_ == nullptr)
			std::copy_n(other.inline_.begin(), size_, inline_.begin());
		other.size_ = 0;
		other.capacity_ = inline_capacity;
		other.heap_ = nullptr;
	}

	ProgramWords &operator=(ProgramWords &&other) noexcept
	{
		if (this == &other)
			return *this;
		release();
		resource_ = other.resource_;
		size_ = other.size_;
		capacity_ = other.capacity_;
		heap_ = other.heap_;
		if (heap_ == nullptr)
			std::copy_n(other.inline_.begin(), size_, inline_.begin());
		other.size_ = 0;
		other.capacity_ = inline_capacity;
		other.heap_ = nullptr;
		return *this;
	}

	~ProgramWords() { release(); }

	void push_back(std::uintptr_t word)
	{
		if (size_ == capacity_)
			reserve(capacity_ == inline_capacity ? 34 : capacity_ * 2);
		data()[size_++] = word;
	}

	void reserve(std::size_t requested)
	{
		if (requested <= capacity_)
			return;
		auto *replacement = static_cast<std::uintptr_t *>(
			resource_->allocate(requested * sizeof(std::uintptr_t),
				alignof(std::uintptr_t)));
		std::copy_n(data(), size_, replacement);
		if (heap_ != nullptr)
			resource_->deallocate(heap_,
				capacity_ * sizeof(std::uintptr_t),
				alignof(std::uintptr_t));
		heap_ = replacement;
		capacity_ = requested;
	}

	[[nodiscard]] std::uintptr_t *data() noexcept
	{
		return heap_ != nullptr ? heap_ : inline_.data();
	}
	[[nodiscard]] const std::uintptr_t *data() const noexcept
	{
		return heap_ != nullptr ? heap_ : inline_.data();
	}
	[[nodiscard]] std::size_t size() const noexcept { return size_; }
	[[nodiscard]] bool empty() const noexcept { return size_ == 0; }
	[[nodiscard]] std::uintptr_t &operator[](std::size_t index) noexcept
	{
		return data()[index];
	}
	[[nodiscard]] const std::uintptr_t &operator[](
		std::size_t index) const noexcept
	{
		return data()[index];
	}

private:
	void release() noexcept
	{
		if (heap_ != nullptr)
			resource_->deallocate(heap_,
				capacity_ * sizeof(std::uintptr_t),
				alignof(std::uintptr_t));
	}

	std::pmr::memory_resource *resource_;
	std::size_t size_{};
	std::size_t capacity_{inline_capacity};
	std::uintptr_t *heap_{};
	std::array<std::uintptr_t, inline_capacity> inline_;
};

class Program final {
public:
	/* Caller, callee, and the continuation after the inlined return. */
	static constexpr std::size_t maximum_runs = 3;

	Program(std::uint64_t guest_address, std::size_t instruction_count,
		ProgramWords words,
		bool accesses_fp_state) noexcept;
	Program(std::span<const GuestRun> runs, std::size_t instruction_count,
		ProgramWords words,
		bool accesses_fp_state) noexcept;
	Program(std::uint64_t guest_address, std::size_t instruction_count,
		std::pmr::vector<std::uintptr_t> words,
		bool accesses_fp_state);
	Program(std::span<const GuestRun> runs, std::size_t instruction_count,
		std::pmr::vector<std::uintptr_t> words,
		bool accesses_fp_state);

	/* Where the block starts, which is the first run's address. */
	[[nodiscard]] std::uint64_t guest_address() const noexcept
	{
		return runs_[0].address;
	}
	[[nodiscard]] std::span<const GuestRun> runs() const noexcept
	{
		return {runs_.data(), run_count_};
	}
	[[nodiscard]] bool contiguous() const noexcept
	{
		return run_count_ == 1;
	}
	[[nodiscard]] std::size_t instruction_count() const noexcept
	{
		return instruction_count_;
	}
	[[nodiscard]] std::span<const std::uintptr_t> words() const noexcept
	{
		return {words_.data(), words_.size()};
	}
	[[nodiscard]] bool accesses_fp_state() const noexcept
	{
		return accesses_fp_state_;
	}

private:
	std::array<GuestRun, maximum_runs> runs_{};
	std::size_t instruction_count_{};
	ProgramWords words_;
	std::uint8_t run_count_{1};
	bool accesses_fp_state_{};
};

/* Where a guest PC sits in a program, counted in retired instructions from its
 * entry. Empty when the PC is not one this program translated, which is how a
 * caller distinguishes "not ours" from position zero. */
[[nodiscard]] inline std::optional<std::size_t> program_position(
	const Program &program, std::uint64_t pc) noexcept
{
	std::size_t base = 0;
	for (const auto &run : program.runs()) {
		const auto span = std::uint64_t{run.instructions} *
			sizeof(std::uint32_t);
		if (pc >= run.address && pc - run.address < span) {
			const auto offset = pc - run.address;
			if ((offset & (sizeof(std::uint32_t) - 1U)) != 0U)
				return std::nullopt;
			return base + static_cast<std::size_t>(
				offset / sizeof(std::uint32_t));
		}
		base += run.instructions;
	}
	return std::nullopt;
}

struct BuildResult final {
	Program program;
	bool complete{};
	std::uint64_t stop_address{};
	std::uint32_t stop_instruction{};
};

/* A conditional branch whose fall-through was translated inline behind it.
 *
 * Entering a block precharges its whole length, so a taken branch leaving from
 * the middle has to give back the instructions after it. That count is not
 * known until the block ends, so each site is recorded here with the number of
 * instructions translated before it, and filled in once the length is final.
 */
struct InlineSuffixes final {
	/* One per conditional branch in a block, which cannot exceed the number
	 * of instructions the fetch window holds. */
	static constexpr std::size_t capacity = 64;

	std::array<std::uint32_t, capacity> word{};
	std::array<std::uint32_t, capacity> compiled{};
	std::uint32_t count{};

	[[nodiscard]] bool full() const noexcept { return count == capacity; }

	void add(std::size_t at, std::size_t translated) noexcept
	{
		word[count] = static_cast<std::uint32_t>(at);
		compiled[count] = static_cast<std::uint32_t>(translated);
		++count;
	}
};

/* A stream cell holding a pointer to link state the executor owns.
 *
 * Two executors translating the same guest words emit identical streams except
 * in these cells: BlockLink and IndirectLink records belong to one executor and
 * one address space, so a translation can be handed to another executor only if
 * every such cell is recorded by the guest PC it names rather than by the
 * pointer it holds, and resolved again against the consumer's own records.
 */
struct LinkRelocation final {
	/* Index into Program::words(). */
	std::uint32_t word{};
	/* False names a BlockLink for the branch target, true an IndirectLink
	 * for the branch site. */
	bool indirect{};
	std::uint64_t guest_pc{};
};

/* A block ends at its first control-flow instruction and no terminal emits more
 * than two link cells, so recording them needs no allocation. A build that
 * somehow exceeds that says so instead of truncating, and an overflowed record
 * is never published. */
struct LinkRelocations final {
	static constexpr std::size_t capacity = 2;

	std::array<LinkRelocation, capacity> entries{};
	std::uint8_t count{};
	bool overflowed{};

	void add(std::size_t word, bool indirect,
		 std::uint64_t guest_pc) noexcept
	{
		if (count == capacity) {
			overflowed = true;
			return;
		}
		entries[count++] = {static_cast<std::uint32_t>(word), indirect,
			guest_pc};
	}
};

struct BuildOptions final {
	/*
	 * Resolve backward-compatible HINT encodings to NOP and reject the
	 * non-HINT allocations when the virtual CPU does not expose the base
	 * extension. Tests default to the complete software implementation;
	 * production disables it once at translation time.
	 */
	bool enable_pointer_authentication{true};
	/* The production guest profile does not currently expose guarded control
	 * flow. Tests enable it explicitly so authenticated calls and returns use
	 * dedicated precompiled entries without adding state checks to the normal
	 * hot path.
	 */
	bool enable_guarded_control_flow{};
	/* Test and portability validation can force optional checksum operations
	 * through their precompiled software gadgets. Production leaves this
	 * false and selects from detected host features.
	 */
	bool force_software_checksum{};
	/* Exercise portable flag-manipulation bodies even on a capable host.
	 * Production selects native fixed-form gadgets at translation time.
	 */
	bool force_software_flags{};
	/* Exercise portable SIMD logical/rotation bodies while leaving the
	 * interpreter and JIT portions of a shared conformance case unchanged.
	 */
	bool force_software_sha3{};
	/* Exercise portable fixed-transform bodies on capable hosts. Production
	 * resolves each optional host family once while translating.
	 */
	bool force_software_fixed_transforms{};
	/* Exercise the portable 128-bit block-transform bodies on a capable
	 * host. Production selects the native path once at translation time.
	 */
	bool force_software_aes{};
	/* Exercise the portable 64-bit polynomial-multiply gadget even on a host
	 * with the optional native instruction. Byte forms remain native because
	 * they are part of baseline AdvSIMD.
	 */
	bool force_software_pmull{};
	/* Exercise the portable integer dot-product bodies on capable hosts.
	 * Production selects native bodies independently for the ordinary and
	 * mixed-sign families from detected host features.
	 */
	bool force_software_dotprod{};
	bool force_software_i8mm{};
	::uml::cpu::HostCapabilities host_capabilities{
		::uml::cpu::host_capabilities()};
};

/* Direct control-flow gadgets load these immutable-layout cells through the
 * data stream. The cache may retarget a cell from the shared exit stream to a
 * translated successor without modifying executable memory. */
struct BlockLink final {
	const std::uintptr_t *stream{};
	std::size_t instruction_count{};
	std::uint64_t guest_pc{};
};
static_assert(offsetof(BlockLink, stream) == 0);
static_assert(offsetof(BlockLink, instruction_count) == 8);
static_assert(offsetof(BlockLink, guest_pc) == 16);
static_assert(sizeof(BlockLink) == 24);

struct IndirectLinkEntry final {
	std::uint64_t guest_pc{};
	const BlockLink *link{};
};
static_assert(offsetof(IndirectLinkEntry, guest_pc) == 0);
static_assert(offsetof(IndirectLinkEntry, link) == 8);
static_assert(sizeof(IndirectLinkEntry) == 16);

/* Two target cells cover the monomorphic and common bimorphic branch/return
 * sites while keeping the assembly hit path compact. ProgramCache owns and
 * trains these data-only cells; translated streams only retain stable
 * pointers to them. */
struct IndirectLink final {
	std::array<IndirectLinkEntry, 2> entries{};
	std::uint8_t next_replacement{};
};
static_assert(offsetof(IndirectLink, entries) == 0);
static_assert(offsetof(IndirectLink, next_replacement) == 32);
static_assert(sizeof(IndirectLink) == 40);

/* What a build needs in order to translate a callee inline: permission for this
 * particular callee, and a way to read guest instructions at an address other
 * than the one the build started from. Absent, calls end the block as before. */
struct InlineCalls final {
	std::function<bool(std::uint64_t)> may_inline;
	std::function<std::size_t(std::uint64_t, std::span<std::uint32_t>)> fetch;
};

class Builder final {
public:
	using LinkResolver =
		std::function<const BlockLink *(std::uint64_t guest_pc)>;
	using IndirectLinkResolver =
		std::function<IndirectLink *(std::uint64_t source_pc)>;

	[[nodiscard]] BuildResult build(
		std::uint64_t guest_address,
		std::span<const std::uint32_t> instructions,
		LinkResolver resolve_link = {},
		std::pmr::memory_resource *storage =
			std::pmr::get_default_resource(),
		IndirectLinkResolver resolve_indirect_link = {},
		BuildOptions options = {},
		LinkRelocations *relocations = nullptr,
		const InlineCalls *inline_calls = nullptr,
		std::span<std::uint32_t> consumed = {},
		std::size_t *consumed_count = nullptr) const;
};

[[nodiscard]] ::uml::cpu::EngineEvent execute(
	const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb) noexcept;
[[nodiscard]] ::uml::cpu::EngineEvent execute(
	const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb,
	std::size_t &executed) noexcept;
[[nodiscard]] ::uml::cpu::EngineEvent
execute_preserving_native_fp_environment(
	const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb,
	std::size_t &executed) noexcept;
using ::uml::cpu::arm64::restore_native_fp_environment;
using ::uml::cpu::arm64::restore_native_fpmr_environment;

} // namespace uml::cpu::arm64::gadgets
