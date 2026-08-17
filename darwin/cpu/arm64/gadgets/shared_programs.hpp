// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "program.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <vector>

namespace uml::cpu::arm64::gadgets {

/* What makes two translations interchangeable.
 *
 * The address, because a gadget program embeds the absolute PC of everything it
 * translates; the guest words the build actually read, because those are what
 * every decision in it came from; and the code-generation profile, because
 * gadget selection reads host capabilities and the software-path overrides.
 *
 * `hash` covers only the address and the profile, so it can be taken before the
 * window is even looked at. Identity is settled against the entry, by comparing
 * the words that entry consumed -- five or six instructions for a typical
 * block, against the sixty-four the fetch window holds. A collision therefore
 * costs a publication rather than handing an executor somebody else's
 * translation.
 */
struct SharedKey final {
	std::uint64_t hash{};
	std::uint64_t guest_pc{};
	std::uint64_t profile{};
	std::span<const std::uint32_t> window;

	[[nodiscard]] static SharedKey of(
		std::uint64_t guest_pc, std::span<const std::uint32_t> window,
		std::uint64_t profile) noexcept;
};

/* Everything about a canonical translation except its stream.
 *
 * The stream is copied straight into the consumer's own storage rather than
 * described here, because nothing may retain a pointer into the store: another
 * executor's publication can evict any entry.
 */
struct SharedTranslation final {
	LinkRelocations relocations;
	std::size_t instruction_count{};
	std::uint64_t stop_address{};
	std::uint32_t stop_instruction{};
	bool complete{};
	bool accesses_fp_state{};
};

struct SharedProgramCounters final {
	std::uint64_t lookups{};
	std::uint64_t adoptions{};
	std::uint64_t publications{};
	std::uint64_t rejections{};
	std::uint64_t evictions{};
	/* Entries displaced because the address now holds different code. */
	std::uint64_t replacements{};
	std::uint64_t entries{};
	std::uint64_t bytes{};
};

/* Translations one executor built, in a form another can adopt.
 *
 * Every vCPU carries its own executor and its own cache, so a guest running on
 * several of them translates the same code once per vCPU. Measured on a browser
 * guest, 69% of all translations at four vCPUs and 80% at eight were work
 * another executor had already done, and the share rises with vCPU count
 * because more executors means more chances someone else got there first.
 *
 * What makes a translation transferable is that its stream is identical between
 * executors apart from the cells naming BlockLink and IndirectLink records,
 * which belong to one executor and one address space. A canonical entry holds
 * zero in those cells and names them by guest PC instead; the consumer resolves
 * them against its own records while copying the stream into its own storage.
 * Executor addresses and mutable link state therefore never enter this store.
 */
class SharedProgramStore final {
public:
	[[nodiscard]] static SharedProgramStore &instance() noexcept;

	/* Copy the canonical stream for `key` into `words`, still holding zero
	 * in its link cells, and describe it in `translation`. */
	[[nodiscard]] bool adopt(const SharedKey &key,
				 std::pmr::vector<std::uintptr_t> &words,
				 SharedTranslation &translation);

	/* Offer a translation. A stream whose link cells could not all be
	 * recorded is refused rather than published without them. */
	void publish(const SharedKey &key, const BuildResult &result,
		     const LinkRelocations &relocations);

	[[nodiscard]] SharedProgramCounters counters() const;

	/* Publication is worthwhile only once somebody could adopt. */
	static void executor_created() noexcept;
	static void executor_destroyed() noexcept;
	[[nodiscard]] static bool sharing_worthwhile() noexcept;

	/* Tests own several executors' lifetimes inside one process and must be
	 * able to start from an empty store. */
	void clear();

	SharedProgramStore(const SharedProgramStore &) = delete;
	SharedProgramStore &operator=(const SharedProgramStore &) = delete;

private:
	SharedProgramStore();
	~SharedProgramStore();

	class Shard;

	std::unique_ptr<Shard[]> shards_;
};

} // namespace uml::cpu::arm64::gadgets
