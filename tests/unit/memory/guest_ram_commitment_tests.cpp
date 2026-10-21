// SPDX-License-Identifier: GPL-2.0-or-later
/* Guest RAM commitment against real anonymous memory: a released range must
 * leave the process footprint, a committed one must be counted (and wired)
 * again, and repeated, overlapping or concurrent calls must reach the host
 * only for pages whose state they change. */
#include "mach_vm_region_test_support.hpp"
#include "memory/guest_ram_commitment.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using uml::memory::GuestRamCommitment;

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UML_COMMITMENT_TEST_UNDER_ASAN 1
#endif
#endif

/* AddressSanitizer intercepts mlock() and does nothing, so no range it
 * builds is ever wired. */
#if defined(UML_COMMITMENT_TEST_UNDER_ASAN)
constexpr const char *wiring_unsupported =
	"AddressSanitizer ignores mlock()";
#else
constexpr const char *wiring_unsupported = nullptr;
#endif

constexpr std::size_t mib = 1024U * 1024U;

using uml::testing::host_page;
using uml::testing::require;
using uml::testing::task_footprint;
using uml::testing::wired_count;

class Mapping final {
public:
	explicit Mapping(std::size_t size) : size_(size)
	{
		void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANON, -1, 0);
		require(mapping != MAP_FAILED, "anonymous mapping failed");
		data_ = static_cast<std::byte *>(mapping);
	}
	~Mapping()
	{
		(void)munlock(data_, size_);
		(void)munmap(data_, size_);
	}
	Mapping(const Mapping &) = delete;
	Mapping &operator=(const Mapping &) = delete;

	[[nodiscard]] std::byte *data() const noexcept { return data_; }

	void fill(std::size_t offset, std::size_t length, std::byte value)
	{
		std::memset(data_ + offset, static_cast<int>(value), length);
	}

private:
	std::byte *data_{};
	std::size_t size_{};
};

void release_leaves_and_commit_restores_the_footprint()
{
	const std::size_t size = 64 * mib;
	Mapping ram{size};
	ram.fill(0, size, std::byte{1});
	GuestRamCommitment commitment{ram.data(), size, 0, size, false,
		host_page()};
	require(commitment.released_bytes() == 0,
		"an unwired host starts with everything committed");

	const std::uint64_t before = task_footprint();
	require(commitment.release(0, size / 2) == 0, "release failed");
	const std::uint64_t released = task_footprint();
	require(before - released >= size / 2 - 4 * mib,
		"a released range must leave the footprint");
	require(commitment.released_bytes() == size / 2,
		"released bytes must count the released range");

	require(commitment.commit(0, size / 2) == 0, "commit failed");
	ram.fill(0, size / 2, std::byte{2});
	require(task_footprint() >= before - 4 * mib,
		"a committed range must be counted again once written");
	require(commitment.released_bytes() == 0, "commit must clear the range");
	for (std::size_t offset = 0; offset < size / 2; offset += host_page())
		require(ram.data()[offset] == std::byte{2},
			"data written after commit must survive");
}

void repeated_and_overlapping_calls_change_state_once()
{
	const std::size_t page = host_page();
	const std::size_t size = 256 * page;
	Mapping ram{size};
	ram.fill(0, size, std::byte{1});
	GuestRamCommitment commitment{ram.data(), size, 0, size, false, page};

	require(commitment.release(0, 64 * page) == 0, "first release failed");
	require(commitment.release(0, 64 * page) == 0, "repeated release failed");
	require(commitment.release(32 * page, 64 * page) == 0,
		"overlapping release failed");
	require(commitment.released_bytes() == 96 * page,
		"overlapping releases must count each page once");
	require(commitment.commit(16 * page, 16 * page) == 0,
		"partial commit failed");
	require(commitment.commit(16 * page, 16 * page) == 0,
		"repeated commit failed");
	require(commitment.released_bytes() == 80 * page,
		"a partial commit must leave the rest released");
	require(commitment.commit(0, size) == 0, "whole commit failed");
	require(commitment.released_bytes() == 0,
		"committing everything must leave nothing released");
}

void ranges_outside_the_managed_window_are_ignored_or_refused()
{
	const std::size_t page = host_page();
	const std::size_t size = 64 * page;
	Mapping ram{size};
	ram.fill(0, size, std::byte{1});
	/* The first 16 pages stand for the signed kernel image. */
	GuestRamCommitment commitment{ram.data(), size, 16 * page, size, false,
		page};
	require(commitment.managed_bytes() == 48 * page,
		"managed bytes must exclude the unmanaged prefix");
	require(commitment.release(0, 32 * page) == 0,
		"a range reaching into the prefix is not an error");
	require(commitment.released_bytes() == 16 * page,
		"only the managed part of a range may be released");

	errno = 0;
	require(commitment.release(1, page) == -1 && errno == EINVAL,
		"a misaligned offset must be refused");
	errno = 0;
	require(commitment.commit(0, page + 1) == -1 && errno == EINVAL,
		"a misaligned length must be refused");
	errno = 0;
	require(commitment.release(size - page, 2 * page) == -1 &&
			errno == EINVAL,
		"a range past the backing must be refused");
}

void a_wired_host_wires_on_commit_and_unwires_on_release()
{
	const std::size_t page = host_page();
	const std::size_t size = 64 * page;
	Mapping ram{size};
	GuestRamCommitment commitment{ram.data(), size, 0, 0, true, page};
	require(commitment.released_bytes() == size,
		"a wired host starts with everything released");
	require(wired_count(ram.data()) == 0, "nothing is wired before commit");

	require(commitment.commit(0, 8 * page) == 0, "commit failed");
	require(wired_count(ram.data()) > 0, "a committed range must be wired");
	require(wired_count(ram.data() + 16 * page) == 0,
		"an uncommitted range must stay unwired");
	require(commitment.release(0, 8 * page) == 0, "release failed");
	require(wired_count(ram.data()) == 0,
		"a released range must be unwired");
	require(commitment.wire_failures() == 0, "wiring must not have failed");
}

void concurrent_callers_keep_the_count_exact()
{
	const std::size_t page = host_page();
	constexpr std::size_t threads = 8;
	constexpr std::size_t chunk_pages = 32;
	const std::size_t size = threads * 8 * chunk_pages * page;
	Mapping ram{size};
	ram.fill(0, size, std::byte{1});
	GuestRamCommitment commitment{ram.data(), size, 0, size, false, page};

	std::vector<std::thread> workers;
	for (std::size_t worker = 0; worker < threads; ++worker) {
		workers.emplace_back([&, worker] {
			for (unsigned round = 0; round < 200; ++round) {
				/* Neighbouring workers share words of the state map. */
				const std::size_t chunk = (round * threads + worker) %
					(size / (chunk_pages * page));
				const std::uint64_t offset =
					chunk * chunk_pages * page;
				(void)commitment.release(offset, chunk_pages * page);
				(void)commitment.commit(offset, chunk_pages * page);
			}
		});
	}
	for (auto &worker : workers)
		worker.join();
	require(commitment.commit(0, size) == 0, "final commit failed");
	require(commitment.released_bytes() == 0,
		"concurrent release and commit must balance");
}

} // namespace

int main()
{
	const uml::testing::NamedTest tests[] = {
		{"release leaves and commit restores the footprint",
			release_leaves_and_commit_restores_the_footprint},
		{"repeated and overlapping calls change state once",
			repeated_and_overlapping_calls_change_state_once},
		{"ranges outside the managed window are ignored or refused",
			ranges_outside_the_managed_window_are_ignored_or_refused},
		{"a wired host wires on commit and unwires on release",
			a_wired_host_wires_on_commit_and_unwires_on_release,
			wiring_unsupported},
		{"concurrent callers keep the count exact",
			concurrent_callers_keep_the_count_exact},
	};
	return uml::testing::runNamedTests(tests);
}
