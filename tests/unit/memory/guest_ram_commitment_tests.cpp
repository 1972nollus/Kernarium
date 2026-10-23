// SPDX-License-Identifier: GPL-2.0-or-later
/* Guest RAM commitment against real anonymous memory: a released range must
 * leave the process footprint, a committed one must be counted (and wired)
 * again, and repeated, overlapping or concurrent calls must reach the host
 * only for pages whose state they change. A wire must also survive the
 * signals guest CPU threads take while they commit. */
#include "mach_vm_region_test_support.hpp"
#include "memory/guest_ram_commitment.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
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

/* A shared file mapping of pages the host has never cached: a wire of it
 * reads every page from storage, so the wire's faults wait, as they wait for
 * memory on a device under pressure. */
class ColdFile final {
public:
	explicit ColdFile(std::size_t size) : size_(size)
	{
		std::string path =
			uml::testing::scratch_template("cold-wire-XXXXXX");
		descriptor_ = mkstemp(path.data());
		require(descriptor_ >= 0, "cannot create the file to wire");
		(void)unlink(path.c_str());
		require(fcntl(descriptor_, F_NOCACHE, 1) == 0,
			"cannot keep the file out of the host cache");
		const std::vector<std::byte> block(mib, std::byte{0x5a});
		for (std::size_t offset = 0; offset < size; offset += mib)
			require(pwrite(descriptor_, block.data(), mib,
					static_cast<off_t>(offset)) ==
					static_cast<ssize_t>(mib),
				"cannot fill the file to wire");
		require(fsync(descriptor_) == 0,
			"cannot flush the file to wire");
		void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE,
			MAP_SHARED, descriptor_, 0);
		require(mapping != MAP_FAILED, "cannot map the file to wire");
		data_ = static_cast<std::byte *>(mapping);
	}
	~ColdFile()
	{
		(void)munlock(data_, size_);
		(void)munmap(data_, size_);
		(void)close(descriptor_);
	}
	ColdFile(const ColdFile &) = delete;
	ColdFile &operator=(const ColdFile &) = delete;

	[[nodiscard]] std::byte *data() const noexcept { return data_; }

private:
	std::byte *data_{};
	std::size_t size_{};
	int descriptor_{-1};
};

std::atomic<unsigned long> signals_taken{0};

void take_signal(int) noexcept
{
	signals_taken.fetch_add(1, std::memory_order_relaxed);
}

/* Posts SIGUSR1 to one thread every few microseconds while it lives, as
 * guest CPUs post their IPI wake signal to one another. The handler stays
 * installed, so a signal still pending when the storm ends is harmless. */
class SignalStorm final {
public:
	explicit SignalStorm(pthread_t target)
	{
		struct sigaction action {};
		action.sa_handler = take_signal;
		sigemptyset(&action.sa_mask);
		require(sigaction(SIGUSR1, &action, nullptr) == 0,
			"cannot catch SIGUSR1");
		sender_ = std::thread([this, target] {
			while (!stopping_.load(std::memory_order_relaxed)) {
				(void)pthread_kill(target, SIGUSR1);
				std::this_thread::sleep_for(
					std::chrono::microseconds{20});
			}
		});
	}
	~SignalStorm()
	{
		stopping_.store(true, std::memory_order_relaxed);
		sender_.join();
	}
	SignalStorm(const SignalStorm &) = delete;
	SignalStorm &operator=(const SignalStorm &) = delete;

private:
	std::atomic<bool> stopping_{false};
	std::thread sender_;
};

constexpr std::size_t cold_size = 8 * mib;
constexpr unsigned cold_attempts = 4;

/* XNU ends a wire whose faults wait when a signal reaches the thread, and
 * mlock then fails with ENOMEM. This is the hazard the commitment guards
 * against; if a host stops showing it, the guard and this case need a new
 * look rather than a quiet pass. */
void a_signalled_wire_of_cold_pages_fails_with_enomem()
{
	bool ended = false;
	int error = 0;
	SignalStorm storm{pthread_self()};
	for (unsigned attempt = 0; attempt < cold_attempts && !ended;
	     ++attempt) {
		const ColdFile file{cold_size};
		errno = 0;
		ended = mlock(file.data(), cold_size) != 0;
		error = errno;
	}
	require(ended && error == ENOMEM,
		"a signalled mlock of cold pages must fail with ENOMEM");
}

/* Guest CPU threads take IPI signals while they commit; a commit, and the
 * readmission that ends a hibernation, must wire regardless. */
void a_signal_does_not_cost_a_commit_its_wiring()
{
	const std::size_t page = host_page();
	const unsigned long before =
		signals_taken.load(std::memory_order_relaxed);
	SignalStorm storm{pthread_self()};
	for (unsigned attempt = 0; attempt < cold_attempts; ++attempt) {
		const ColdFile committed{cold_size};
		GuestRamCommitment commitment{committed.data(), cold_size, 0, 0,
			true, page};
		require(commitment.commit(0, cold_size) == 0,
			"a signalled commit must succeed");
		require(commitment.wire_failures() == 0,
			"a signalled commit must wire");
		require(wired_count(committed.data()) > 0,
			"a signalled commit must leave its range wired");

		const ColdFile readmitted{cold_size};
		GuestRamCommitment held{readmitted.data(), cold_size, 0,
			cold_size, true, page};
		require(held.evict(0, cold_size / page) == 0, "evict failed");
		require(held.readmit(0, cold_size / page) == 0,
			"readmit failed");
		require(held.wire_failures() == 0,
			"a signalled readmission must wire");
		require(wired_count(readmitted.data()) > 0,
			"a signalled readmission must leave its range wired");
	}
	require(signals_taken.load(std::memory_order_relaxed) > before,
		"the committing thread must have taken signals");
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
		{"a signalled wire of cold pages fails with ENOMEM",
			a_signalled_wire_of_cold_pages_fails_with_enomem,
			wiring_unsupported},
		{"a signal does not cost a commit its wiring",
			a_signal_does_not_cost_a_commit_its_wiring,
			wiring_unsupported},
	};
	return uml::testing::runNamedTests(tests);
}
