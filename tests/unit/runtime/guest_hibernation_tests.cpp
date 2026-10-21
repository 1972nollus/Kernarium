// SPDX-License-Identifier: GPL-2.0-or-later
/* The order of a hibernation, against steps that record it, and the same
 * hibernation against real threads and a real guest RAM. The order is the
 * contract: a thread is held before any page leaves, and no thread is let go
 * before every page is back and verified. */
#include "guest_hibernation.hpp"
#include "kernel_threads.hpp"
#include "memory/guest_ram_commitment.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"
#include "thread_nap.hpp"
#include "watchdog.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace uml::runtime;
using uml::testing::nap;
using uml::testing::require;

enum class Event { prepare, hold, write, restore, release, abort_on, abort_off,
	failure };

std::vector<Event> events;

/* What each fake step answers, and what it checks of the others. */
struct Script final {
	bool prepare{true};
	bool hold{true};
	bool write{true};
	bool restore{true};
};

class RecordingSteps final : public HibernationSteps {
public:
	explicit RecordingSteps(Script script) : script_(script) {}

	bool prepare() noexcept override
	{
		events.push_back(Event::prepare);
		require(!held_, "prepared while threads are held");
		return script_.prepare;
	}

	bool hold(HibernationReport &) noexcept override
	{
		events.push_back(Event::hold);
		require(!held_, "held twice");
		held_ = script_.hold;
		return script_.hold;
	}

	bool write(HibernationReport &) noexcept override
	{
		events.push_back(Event::write);
		require(held_, "RAM written out while threads still run");
		return script_.write;
	}

	bool restore(HibernationReport &) noexcept override
	{
		events.push_back(Event::restore);
		require(held_, "RAM restored after the threads were let go");
		restored_ = script_.restore;
		return script_.restore;
	}

	void release() noexcept override
	{
		events.push_back(Event::release);
		require(held_, "released without holding");
		require(restored_, "threads let go before the RAM was whole");
		held_ = false;
	}

	void set_abort(bool abort) noexcept override
	{
		events.push_back(abort ? Event::abort_on : Event::abort_off);
	}

private:
	Script script_;
	bool held_{false};
	bool restored_{false};
};

void the_failure_handler() noexcept
{
	events.push_back(Event::failure);
}

[[nodiscard]] GuestHibernation make(Script script)
{
	return GuestHibernation{std::make_unique<RecordingSteps>(script),
		&the_failure_handler};
}

void require_events(std::initializer_list<Event> expected, const char *message)
{
	require(events == std::vector<Event>(expected), message);
	events.clear();
}

void a_hibernation_holds_before_it_writes_and_restores_before_it_releases()
{
	events.clear();
	GuestHibernation hibernation = make({});
	require(hibernation.hibernate(), "the hibernation must take place");
	require(hibernation.hibernated(), "and say so");
	require_events({Event::abort_off, Event::prepare, Event::hold,
		Event::write}, "hibernate: prepare, hold, then write");
	require(hibernation.restore(), "the restore must succeed");
	require(!hibernation.hibernated(), "and the guest is whole again");
	require_events({Event::restore, Event::release},
		"restore: the RAM comes back before any thread is let go");
}

void nothing_happens_when_there_is_nothing_to_hibernate()
{
	events.clear();
	GuestHibernation hibernation = make({.prepare = false});
	require(!hibernation.hibernate(), "no hibernation");
	require_events({Event::abort_off, Event::prepare},
		"nothing but the preparation ran");
	require(hibernation.restore(), "a restore of nothing succeeds");
	require_events({}, "and touches nothing");
}

void threads_that_cannot_be_held_leave_the_ram_alone()
{
	events.clear();
	GuestHibernation hibernation = make({.hold = false});
	require(!hibernation.hibernate(), "no hibernation");
	require(!hibernation.hibernated(), "and nothing to restore");
	require_events({Event::abort_off, Event::prepare, Event::hold},
		"the RAM is not written when the threads are not held");
	require(hibernation.restore(), "a restore of nothing succeeds");
	require_events({}, "and touches nothing");
}

void a_partial_write_is_still_restored()
{
	events.clear();
	GuestHibernation hibernation = make({.write = false});
	require(hibernation.hibernate(), "what was evicted must come back");
	require(hibernation.hibernated(), "so the guest is hibernated");
	events.clear();
	require(hibernation.restore(), "the restore succeeds");
	require_events({Event::restore, Event::release}, "and runs in order");
}

void a_restore_that_fails_lets_nothing_run()
{
	events.clear();
	GuestHibernation hibernation = make({.restore = false});
	require(hibernation.hibernate(), "hibernated");
	events.clear();
	require(!hibernation.restore(), "the restore must fail");
	require_events({Event::restore, Event::failure},
		"the failure ends the process; no thread is let go");
	require(hibernation.hibernated(), "and the guest stays hibernated");
}

void an_abort_request_reaches_the_steps()
{
	events.clear();
	GuestHibernation hibernation = make({});
	hibernation.request_abort();
	require_events({Event::abort_on}, "the request reaches the steps");
	require(hibernation.hibernate(), "a hibernation");
	require(events.front() == Event::abort_off,
		"a hibernation starts with the request cleared");
	events.clear();
	require(hibernation.restore(), "restored");
	events.clear();
}

void the_policy_follows_the_mode_and_the_wiring()
{
	require(hibernation_applies(HibernationMode::automatic, true),
		"a wired guest hibernates by default");
	require(!hibernation_applies(HibernationMode::automatic, false),
		"an unwired guest does not");
	require(!hibernation_applies(HibernationMode::off, true), "off is off");
	require(hibernation_applies(HibernationMode::on, false), "on forces it");
}

/* ---- Real threads and real guest RAM. ---- */

constexpr std::size_t held_pages = 192;
constexpr std::size_t spare_pages = 64;

struct Ram final {
	std::byte *root{};
	std::size_t page{};
	std::size_t size{};
	uml::memory::GuestRamCommitment *commitment{};
	std::atomic<std::uint64_t> pattern_errors{0};
	std::atomic<std::uint64_t> ticks{0};
	std::atomic<std::uint64_t> changes{0};
	std::atomic<bool> stop{false};
};

[[nodiscard]] std::uint64_t word_of(std::size_t page, std::size_t word)
{
	return (page + 1) * 0x9e3779b97f4a7c15ULL ^ (word * 0xff51afd7ed558ccdULL);
}

/* A kernel thread: it reads guest RAM, and changes what the guest holds, as
 * the kernel does. It makes no other call that a hold could be stuck in. */
void *kernel_body(void *argument)
{
	auto *ram = static_cast<Ram *>(argument);
	std::size_t round = 0;
	while (!ram->stop.load(std::memory_order_acquire)) {
		const std::size_t page = round++ % held_pages;
		const auto *words = reinterpret_cast<const std::uint64_t *>(
			ram->root + page * ram->page);
		for (std::size_t word = 0; word < ram->page / 8; word += 97)
			if (words[word] != word_of(page, word))
				ram->pattern_errors.fetch_add(1);
		/* The spare pages come and go, as a balloon would have them. */
		const std::uint64_t offset = (held_pages + round % spare_pages) *
			ram->page;
		if ((round & 1) != 0)
			(void)ram->commitment->release(offset, ram->page);
		else
			(void)ram->commitment->commit(offset, ram->page);
		ram->changes.fetch_add(1);
		ram->ticks.fetch_add(1, std::memory_order_relaxed);
		nap(50);
	}
	return nullptr;
}

void wait_for_ticks(const Ram &ram)
{
	const auto before = ram.ticks.load();
	for (int attempt = 0; attempt < 1000; ++attempt) {
		if (ram.ticks.load() > before + 3)
			return;
		nap(2000);
	}
	uml::testing::fail("the kernel thread does not run");
}

[[nodiscard]] bool stood_still(const Ram &ram)
{
	const auto ticks = ram.ticks.load();
	const auto changes = ram.changes.load();
	nap(40000);
	return ram.ticks.load() == ticks && ram.changes.load() == changes;
}

void a_real_hibernation_holds_the_kernel_threads_and_brings_the_ram_back()
{
	Ram ram;
	ram.page = static_cast<std::size_t>(getpagesize());
	ram.size = (held_pages + spare_pages) * ram.page;
	void *mapping = mmap(nullptr, ram.size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	require(mapping != MAP_FAILED, "mapping");
	ram.root = static_cast<std::byte *>(mapping);
	for (std::size_t page = 0; page < held_pages; ++page) {
		auto *words = reinterpret_cast<std::uint64_t *>(
			ram.root + page * ram.page);
		for (std::size_t word = 0; word < ram.page / 8; ++word)
			words[word] = word_of(page, word);
	}
	auto commitment = std::make_unique<uml::memory::GuestRamCommitment>(
		ram.root, ram.size, 0, 0, true, ram.page);
	ram.commitment = commitment.get();
	uml::memory::publish_guest_ram_commitment(std::move(commitment));
	require(ram.commitment->commit(0, ram.size) == 0, "commit");

	const std::string directory = uml::testing::scratch_directory().string();
	std::unique_ptr<GuestHibernation> hibernation =
		GuestHibernation::create(HibernationMode::on, directory);
	pthread_t thread;
	require(kernel_pthread_create(&thread, nullptr, &kernel_body, &ram) == 0,
		"the kernel thread");
	wait_for_ticks(ram);

	for (int cycle = 0; cycle < 6; ++cycle) {
		require(hibernation->hibernate(), "the hibernation must take place");
		const HibernationReport &report = hibernation->report();
		require(report.threads_held == 1 && report.threads_registered == 1,
			"the kernel thread is the one thread held");
		require(report.written.evicted_bytes != 0, "RAM left the process");
		require(stood_still(ram), "the kernel thread runs while hibernated");
		const std::string image = directory + "/" +
			uml::memory::guest_ram_image_name(static_cast<int>(getpid()));
		require(std::filesystem::exists(image), "the image file");
		require(hibernation->restore(), "the restore must succeed");
		require(hibernation->report().restored.chunks_failed == 0,
			"no chunk failed");
		wait_for_ticks(ram);
	}
	require(ram.pattern_errors.load() == 0,
		"the kernel thread saw guest RAM that was not whole");
	ram.stop.store(true);
	require(pthread_join(thread, nullptr) == 0, "join");
	for (std::size_t page = 0; page < held_pages; ++page) {
		const auto *words = reinterpret_cast<const std::uint64_t *>(
			ram.root + page * ram.page);
		for (std::size_t word = 0; word < ram.page / 8; ++word)
			require(words[word] == word_of(page, word),
				"guest RAM differs after the cycles");
	}
	hibernation.reset();
	require(!std::filesystem::exists(directory + "/" +
		uml::memory::guest_ram_image_name(static_cast<int>(getpid()))),
		"the image file must not outlive the hibernation");
}

} // namespace

int main()
{
	const uml::testing::Watchdog watchdog{"the guest hibernation tests",
		std::chrono::seconds{180}};
	const uml::testing::NamedTest tests[] = {
		{"a hibernation holds before it writes and restores before it releases",
			a_hibernation_holds_before_it_writes_and_restores_before_it_releases},
		{"nothing happens when there is nothing to hibernate",
			nothing_happens_when_there_is_nothing_to_hibernate},
		{"threads that cannot be held leave the RAM alone",
			threads_that_cannot_be_held_leave_the_ram_alone},
		{"a partial write is still restored", a_partial_write_is_still_restored},
		{"a restore that fails lets nothing run",
			a_restore_that_fails_lets_nothing_run},
		{"an abort request reaches the steps", an_abort_request_reaches_the_steps},
		{"the policy follows the mode and the wiring",
			the_policy_follows_the_mode_and_the_wiring},
		{"a real hibernation holds the kernel threads and brings the RAM back",
			a_real_hibernation_holds_the_kernel_threads_and_brings_the_ram_back},
	};
	return uml::testing::runNamedTests(tests);
}
