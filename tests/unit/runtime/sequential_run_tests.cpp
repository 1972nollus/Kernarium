// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A process runs one guest, and that guest may restart itself. A kernel
 * claims guest RAM early in its boot, and the host hands guest RAM out once
 * per process. After a run has claimed it, every later run in the process, on
 * the same handle or a fresh one, returns -UML_EMULATOR_UNSUPPORTED without
 * entering a kernel, and the process goes on, unless the run ended with a
 * restart: the next run on that handle hands the guest back to the process and
 * boots again. A run whose kernel returns before claiming guest RAM leaves the
 * guest for a later run.
 *
 * Each mode runs in its own process because the claim outlives every handle.
 * The restart modes boot a test kernel that does what a real one leaves
 * behind (descriptors, pthread keys, allocations and mappings through the
 * imports the loader follows, changed signal state, a guest RAM root over the
 * process's window, kernel threads, a guest file) and check, at the moment each
 * hand-back completes, that nothing of it is left and that the process is as it
 * was before the first run.
 */
#include "uml/emulator.h"
#include "context.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "cpu/gadget_cache_budget.hpp"
#include "device/sockets/socket_paths.hpp"
#include "emulator_test_support.hpp"
#include "guest_files.hpp"
#include "kernel_resources.hpp"
#include "kernel_threads.hpp"
#include "mach_vm_region_test_support.hpp"
#include "memory/guest_ram_commitment.hpp"
#include "memory/mmu/domain_registry_testing.hpp"
#include "memory/shared_backing.hpp"
#include "native_kernel_layout.hpp"
#include "require.hpp"
#include "testing.hpp"
#include "uml_darwin_host.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <malloc/malloc.h>
#include <memory>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kernel_booted = 41;
constexpr int kernel_without_memory = 42;
constexpr int kernel_left_memory = 43;

/* Kernel entries in this process, and whether the next one claims RAM. */
int kernel_entries;
bool claim_guest_ram = true;

using ::uml::runtime::GuestShutdownKind;
using uml::emulator::testing::GuestHandBackReport;
using uml::testing::make_emulator;
using uml::testing::require;

/* Stands in for the start of a UML boot: setup_physmem() opens guest RAM. */
int kernel(int, char *const[], char *const[])
{
	++kernel_entries;
	if (!claim_guest_ram)
		return kernel_left_memory;
	const int memory = darwin_uml_open("uml:anonymous-physical-memory",
		O_RDWR, 0);
	if (memory < 0)
		return kernel_without_memory;
	(void)::close(memory);
	return kernel_booted;
}

int run(uml_emulator *emulator)
{
	const char *arguments[]{"uml-sequential-runs"};
	return uml_emulator_run(emulator, 1, arguments, nullptr);
}

void require_first_guest(uml_emulator *emulator)
{
	const int entries = kernel_entries;
	require(run(emulator) == kernel_booted,
		"the first guest in the process could not open guest RAM");
	require(kernel_entries == entries + 1,
		"the first guest did not enter its kernel once");
}

void require_refused(uml_emulator *emulator)
{
	const int entries = kernel_entries;
	require(run(emulator) == -UML_EMULATOR_UNSUPPORTED,
		"a run after the process's guest did not fail as unsupported");
	require(kernel_entries == entries,
		"a run after the process's guest entered its kernel");
}

void second_run_on_the_same_handle()
{
	auto emulator = make_emulator(kernel);
	require_first_guest(emulator.get());
	require_refused(emulator.get());
	require_refused(emulator.get());
}

void second_run_on_a_fresh_handle()
{
	{
		auto first = make_emulator(kernel);
		require_first_guest(first.get());
	}
	auto second = make_emulator(kernel);
	require_refused(second.get());
}

void run_that_left_guest_ram()
{
	auto emulator = make_emulator(kernel);
	claim_guest_ram = false;
	require(run(emulator.get()) == kernel_left_memory,
		"a kernel that left guest RAM did not return its status");
	claim_guest_ram = true;
	require_first_guest(emulator.get());
	require_refused(make_emulator(kernel).get());
}

/* ---- A guest that restarts itself ---- */

constexpr std::size_t ram_pages = 64;
constexpr std::size_t image_pages = 4;
/* One past the signals Darwin names that a test can query. */
constexpr int signal_count = 32;

[[nodiscard]] std::size_t host_page() noexcept
{
	return static_cast<std::size_t>(getpagesize());
}

/* The kernel's imports as the loader binds them. */
struct Imports final {
	int (*open)(const char *, int, int);
	int (*close)(int);
	int (*fcntl)(int, int, long);
	int (*socket)(int, int, int);
	int (*socketpair)(int, int, int, int[2]);
	int (*pipe)(int[2]);
	int (*kqueue)();
	int (*dup)(int);
	ssize_t (*recvmsg)(int, msghdr *, int);
	void *(*malloc)(std::size_t);
	void (*free)(void *);
	void *(*mmap)(void *, std::size_t, int, int, int, off_t);
	int (*munmap)(void *, std::size_t);
	void *(*sbrk)(int);
	int (*key_create)(pthread_key_t *, void (*)(void *));
};

template<typename Function>
[[nodiscard]] Function *import(const char *name)
{
	void *const wrapper = uml::runtime::kernel_resource_import(name);
	require(wrapper != nullptr, "the loader does not follow an import");
	return reinterpret_cast<Function *>(wrapper);
}

[[nodiscard]] Imports bind_imports()
{
	return {
		.open = import<int(const char *, int, int)>("darwin_uml_open"),
		.close = import<int(int)>("darwin_uml_close"),
		.fcntl = import<int(int, int, long)>("darwin_uml_fcntl"),
		.socket = import<int(int, int, int)>("socket"),
		.socketpair = import<int(int, int, int, int[2])>("_socketpair"),
		.pipe = import<int(int[2])>("pipe"),
		.kqueue = import<int()>("kqueue"),
		.dup = import<int(int)>("dup"),
		.recvmsg = import<ssize_t(int, msghdr *, int)>("_recvmsg"),
		.malloc = import<void *(std::size_t)>("malloc"),
		.free = import<void(void *)>("free"),
		.mmap = import<void *(void *, std::size_t, int, int, int, off_t)>(
			"_mmap"),
		.munmap = import<int(void *, std::size_t)>("_munmap"),
		.sbrk = import<void *(int)>("sbrk"),
		.key_create = import<int(pthread_key_t *, void (*)(void *))>(
			"pthread_key_create"),
	};
}

/* What the process looks like from outside the guest: the state a kernel
 * changes and the library must give back. */
struct ProcessState final {
	std::array<struct sigaction, signal_count> actions{};
	std::array<bool, signal_count> readable{};
	itimerval timer{};
	sigset_t mask{};
	stack_t signal_stack{};
	std::array<int, 3> standard_flags{};
	std::size_t software_domains{};
	std::size_t native_domains{};
	std::size_t gadget_accounts{};
	std::size_t gadget_bytes{};
};

constexpr int settable_status_flags =
	O_NONBLOCK | O_APPEND | O_ASYNC | O_FSYNC | O_DSYNC;

[[nodiscard]] ProcessState capture_process_state()
{
	ProcessState state;
	for (int number = 1; number < signal_count; ++number)
		state.readable[number] =
			sigaction(number, nullptr, &state.actions[number]) == 0;
	require(getitimer(ITIMER_REAL, &state.timer) == 0,
		"could not read ITIMER_REAL");
	require(pthread_sigmask(SIG_BLOCK, nullptr, &state.mask) == 0,
		"could not read the signal mask");
	require(sigaltstack(nullptr, &state.signal_stack) == 0,
		"could not read the signal stack");
	for (int descriptor = 0; descriptor < 3; ++descriptor)
		state.standard_flags[descriptor] =
			fcntl(descriptor, F_GETFL) & settable_status_flags;
	state.software_domains = uml::memory::testing::software_domain_count();
	state.native_domains = uml::memory::testing::native_domain_count();
	const auto gadgets = uml::cpu::gadget_cache::ledger_state();
	state.gadget_accounts = gadgets.accounts;
	state.gadget_bytes = gadgets.total;
	return state;
}

[[nodiscard]] bool same_mask(const sigset_t &first, const sigset_t &second)
{
	return std::memcmp(&first, &second, sizeof(first)) == 0;
}

void require_same_process(const ProcessState &before)
{
	const auto now = capture_process_state();
	for (int number = 1; number < signal_count; ++number) {
		require(before.readable[number] == now.readable[number],
			"a signal became unreadable");
		if (!before.readable[number])
			continue;
		const auto &was = before.actions[number];
		const auto &is = now.actions[number];
		require(was.sa_handler == is.sa_handler &&
			was.sa_flags == is.sa_flags &&
			same_mask(was.sa_mask, is.sa_mask),
			"a signal disposition was not restored");
	}
	require(now.timer.it_value.tv_sec == before.timer.it_value.tv_sec &&
		now.timer.it_value.tv_usec == before.timer.it_value.tv_usec &&
		now.timer.it_interval.tv_sec == before.timer.it_interval.tv_sec,
		"ITIMER_REAL was left armed");
	require(same_mask(before.mask, now.mask),
		"the run thread's signal mask was not restored");
	/* A disabled stack has no address worth comparing. */
	const bool disabled = (before.signal_stack.ss_flags & SS_DISABLE) != 0;
	require((now.signal_stack.ss_flags & SS_DISABLE) ==
		(before.signal_stack.ss_flags & SS_DISABLE) &&
		(disabled || (before.signal_stack.ss_sp == now.signal_stack.ss_sp &&
		before.signal_stack.ss_size == now.signal_stack.ss_size)),
		"the run thread's signal stack was not restored");
	require(before.standard_flags == now.standard_flags,
		"the status flags of descriptors 0-2 were not restored");
	require(now.software_domains == before.software_domains &&
		now.native_domains == before.native_domains,
		"an address space or generation outlived its run");
	require(now.gadget_accounts == before.gadget_accounts &&
		now.gadget_bytes == before.gadget_bytes,
		"a gadget cache account outlived its run");
	const auto monitor =
		uml::cpu::arm64::jit::testing::exclusive_monitor_state_for_testing();
	require(monitor.active_reservations == 0 && monitor.reservations == 0 &&
		monitor.store_participants == 0 && monitor.gate_quiescent,
		"the JIT exclusive monitor is not empty");
}

/* How the kernel of a boot ends. */
enum class Ending { restart, power_off, halt, stopped, failed };

struct Restarts final {
	uml_emulator *handle{};
	/* Boots 1 .. restarting end with a restart; the next ends as `last`. */
	int restarting{};
	Ending last{Ending::power_off};
	/* The first boot leaves a kernel thread running. */
	bool leave_thread{};
	/* The process before the first run. */
	ProcessState before;
	/* What the last boot created, for the hook to check. */
	GuestHandBackReport expected;
	std::vector<int> descriptors;
	int token{-1};
	int control{-1};
	void *outside_mapping{};
	/* Hand-backs seen, and the default zone's size at each. */
	int hand_backs{};
	std::vector<std::size_t> zone_in_use;
	std::size_t zone_allocated{};
	pthread_t held_thread{};
	std::atomic<bool> release_thread{false};
	bool thread_started{};
} world;

void key_destructor(void *) {}

void signal_tick(int) {}

void *held_kernel_thread(void *) noexcept
{
	while (!world.release_thread.load(std::memory_order_acquire))
		usleep(1000);
	return nullptr;
}

void *short_kernel_thread(void *) noexcept
{
	return nullptr;
}

/* The state a kernel changes that is not its own: the library gives it back
 * when the kernel returns. */
void perturb_process(const Imports &kernel)
{
	struct sigaction ignore {};
	ignore.sa_handler = SIG_IGN;
	sigemptyset(&ignore.sa_mask);
	for (const int number : {SIGPIPE, SIGIO, SIGWINCH})
		require(sigaction(number, &ignore, nullptr) == 0,
			"could not ignore a signal");
	struct sigaction handler {};
	handler.sa_handler = signal_tick;
	handler.sa_flags = SA_RESTART | SA_ONSTACK;
	sigemptyset(&handler.sa_mask);
	for (const int number : {SIGALRM, SIGUSR2})
		require(sigaction(number, &handler, nullptr) == 0,
			"could not install a handler");
	itimerval timer{};
	timer.it_value.tv_sec = 3600;
	timer.it_interval.tv_sec = 3600;
	require(setitimer(ITIMER_REAL, &timer, nullptr) == 0,
		"could not arm ITIMER_REAL");
	sigset_t blocked;
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGALRM);
	sigaddset(&blocked, SIGUSR1);
	require(pthread_sigmask(SIG_BLOCK, &blocked, nullptr) == 0,
		"could not block signals");
	constexpr std::size_t stack_bytes = 128U * 1024U;
	void *const stack = kernel.malloc(stack_bytes);
	require(stack != nullptr, "could not allocate a signal stack");
	stack_t alternate{};
	alternate.ss_sp = stack;
	alternate.ss_size = stack_bytes;
	require(sigaltstack(&alternate, nullptr) == 0,
		"could not set a signal stack");
	const int flags = fcntl(STDIN_FILENO, F_GETFL);
	if (flags >= 0)
		require(fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == 0,
			"could not make stdin non-blocking");
}

void remember(int descriptor)
{
	require(descriptor >= 0, "a kernel import returned no descriptor");
	world.descriptors.push_back(descriptor);
}

/* The rights a kernel receives by recvmsg: a descriptor sent over a socket
 * pair, arriving as a new one. */
int receive_rights(const Imports &kernel, int sender, int receiver,
	int shared)
{
	char byte = 'r';
	iovec vector{.iov_base = &byte, .iov_len = 1};
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
	msghdr message{};
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	auto *header = CMSG_FIRSTHDR(&message);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(int));
	std::memcpy(CMSG_DATA(header), &shared, sizeof(shared));
	require(sendmsg(sender, &message, 0) == 1, "could not send rights");
	char received = 0;
	iovec incoming{.iov_base = &received, .iov_len = 1};
	alignas(cmsghdr) char incoming_control[CMSG_SPACE(sizeof(int))]{};
	msghdr reply{};
	reply.msg_iov = &incoming;
	reply.msg_iovlen = 1;
	reply.msg_control = incoming_control;
	reply.msg_controllen = sizeof(incoming_control);
	require(kernel.recvmsg(receiver, &reply, 0) == 1,
		"could not receive rights");
	int descriptor = -1;
	std::memcpy(&descriptor, CMSG_DATA(CMSG_FIRSTHDR(&reply)),
		sizeof(descriptor));
	return descriptor;
}

[[nodiscard]] bool nothing_present(const void *address, std::size_t length)
{
	std::vector<char> residency(length / host_page());
	require(mincore(const_cast<void *>(address), length,
		residency.data()) == 0, "mincore failed");
	for (const char page : residency)
		if ((page & (MINCORE_INCORE | MINCORE_PAGED_OUT)) != 0)
			return false;
	return true;
}

/* The guest RAM root a kernel maps over the process's window: an image
 * prefix, then the suffix the root's mapping covers. */
void claim_ram(int token, int boot)
{
	const auto window = uml::runtime::process_native_window();
	const std::size_t page = host_page();
	require(window.end - window.begin >= ram_pages * page,
		"the process's window is too small for the test RAM");
	require(ftruncate(token, static_cast<off_t>(ram_pages * page)) == 0,
		"could not size the RAM token");
	auto *const root = reinterpret_cast<std::uint8_t *>(window.begin);
	auto *const suffix = root + image_pages * page;
	const std::size_t suffix_bytes = (ram_pages - image_pages) * page;
	/* The window holds none of the previous guest's RAM. */
	require(nothing_present(root, ram_pages * page),
		"a page of the window is present before the guest maps its RAM");
	require(uml::memory::guest_ram_commitment() == nullptr,
		"a commitment outlived the previous guest");
	void *const mapped = uml::memory::map_shared_backing(suffix, suffix_bytes,
		PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, token,
		image_pages * page);
	require(mapped == suffix, "the guest RAM was not mapped in place");
	require(uml::memory::guest_ram_commitment() != nullptr,
		"the guest RAM published no commitment");
	require(nothing_present(suffix, suffix_bytes),
		"the new commitment has a present page before the kernel runs");
	for (std::size_t offset = 0; offset < suffix_bytes; offset += page)
		require(suffix[offset] == 0,
			"the new guest RAM holds a byte of the previous guest");
	for (std::size_t offset = 0; offset < suffix_bytes; offset += page)
		suffix[offset] = static_cast<std::uint8_t>(0xa0 + boot);
}

/* A kernel as it leaves its process: what it created through its imports and
 * does not close, what it changed, the RAM it claimed, the threads it
 * started, and how it says it ended. */
int boot(int, char *const[], char *const[])
{
	const int index = ++kernel_entries;
	const Imports kernel = bind_imports();
	world.descriptors.clear();
	perturb_process(kernel);

	const int token = kernel.open("uml:anonymous-physical-memory", O_RDWR, 0);
	if (token < 0)
		return kernel_without_memory;
	remember(token);
	world.token = token;
	const std::string path = "/tmp/uml-restart-" + std::to_string(getpid()) +
		"-" + std::to_string(index);
	const int file = kernel.open(path.c_str(), O_RDWR | O_CREAT | O_EXCL,
		0600);
	remember(file);
	(void)unlink(path.c_str());
	uml::platform::record_guest_file(file, O_RDWR);

	int pair[2];
	require(kernel.socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
		"socketpair failed");
	remember(pair[0]);
	remember(pair[1]);
	int pipes[2];
	require(kernel.pipe(pipes) == 0, "pipe failed");
	remember(pipes[0]);
	remember(pipes[1]);
	remember(kernel.kqueue());
	const int duplicate = kernel.dup(pipes[0]);
	remember(duplicate);
	remember(kernel.socket(AF_UNIX, SOCK_DGRAM, 0));
	remember(kernel.fcntl(pipes[1], F_DUPFD, 100));
	remember(receive_rights(kernel, pair[0], pair[1], pipes[0]));
	/* A descriptor the kernel closes itself is not the library's to close. */
	require(kernel.close(duplicate) == 0, "close failed");
	world.descriptors.erase(std::find(world.descriptors.begin(),
		world.descriptors.end(), duplicate));

	/* The signal stack perturb_process() allocated, and 100 bytes more; one
	 * more allocation is freed by the kernel itself. */
	std::size_t allocation_bytes = 128U * 1024U + 100U;
	void *const scratch = kernel.malloc(100);
	void *const freed = kernel.malloc(64);
	require(scratch != nullptr && freed != nullptr, "malloc failed");
	kernel.free(freed);
	const std::size_t page = host_page();
	void *const outside = kernel.mmap(nullptr, 4 * page,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	void *const unmapped = kernel.mmap(nullptr, page,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	require(outside != MAP_FAILED && unmapped != MAP_FAILED,
		"mmap failed");
	require(kernel.munmap(unmapped, page) == 0, "munmap failed");
	world.outside_mapping = outside;
	(void)kernel.sbrk(0);
	pthread_key_t first_key;
	pthread_key_t second_key;
	require(kernel.key_create(&first_key, key_destructor) == 0 &&
		kernel.key_create(&second_key, nullptr) == 0,
		"pthread_key_create failed");

	claim_ram(token, index);

	std::array<pthread_t, 5> threads{};
	for (auto &thread : threads)
		require(uml::runtime::kernel_pthread_create(&thread, nullptr,
			short_kernel_thread, nullptr) == 0,
			"could not start a kernel thread");
	for (auto &thread : threads)
		require(pthread_join(thread, nullptr) == 0,
			"could not join a kernel thread");
	if (world.leave_thread && index == 1) {
		require(uml::runtime::kernel_pthread_create(&world.held_thread,
			nullptr, held_kernel_thread, nullptr) == 0,
			"could not start the held kernel thread");
		world.thread_started = true;
	}

	world.expected = {
		.descriptors = world.descriptors.size(),
		.keys = 2,
		.allocations = 2,
		.allocation_bytes = allocation_bytes,
		.mappings = 1,
		.mapping_bytes = 4 * page,
		.sbrk_calls = 1,
		.guest_files = 1,
	};

	const Ending ending = index <= world.restarting ? Ending::restart :
		world.last;
	switch (ending) {
	case Ending::stopped:
		uml_emulator_request_stop(world.handle);
		uml::emulator::testing::report_guest_shutdown(
			GuestShutdownKind::restart, "stopped");
		break;
	case Ending::restart:
		uml::emulator::testing::report_guest_shutdown(
			GuestShutdownKind::restart, "reboot");
		break;
	case Ending::power_off:
		uml::emulator::testing::report_guest_shutdown(
			GuestShutdownKind::power_off, nullptr);
		break;
	case Ending::halt:
		uml::emulator::testing::report_guest_shutdown(
			GuestShutdownKind::halt, nullptr);
		break;
	case Ending::failed:
		break;
	}
	return kernel_booted;
}

/* The cleanup tables for the socket paths and directories a run binds: a slot
 * that a run kept would show as a table that cannot hold a full set. */
void require_socket_path_tables_empty()
{
	namespace fs = std::filesystem;
	const fs::path directory =
		"/tmp/uml-restart-paths-" + std::to_string(getpid());
	fs::create_directory(directory);
	{
		std::vector<std::unique_ptr<uml::device::SocketPath>> paths;
		std::vector<int> sockets;
		for (int index = 0; index < 16; ++index) {
			const fs::path path = directory / ("s" + std::to_string(index));
			sockaddr_un address{};
			address.sun_family = AF_UNIX;
			std::strncpy(address.sun_path, path.c_str(),
				sizeof(address.sun_path) - 1);
			const int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
			require(descriptor >= 0 &&
				bind(descriptor, reinterpret_cast<sockaddr *>(&address),
					sizeof(address)) == 0, "could not bind a socket");
			sockets.push_back(descriptor);
			paths.push_back(std::make_unique<uml::device::SocketPath>(path));
			paths.back()->adopt();
		}
		std::vector<std::unique_ptr<uml::device::DirectoryPath>> directories;
		for (int index = 0; index < 4; ++index) {
			const fs::path path = directory / ("d" + std::to_string(index));
			fs::create_directory(path);
			directories.push_back(
				std::make_unique<uml::device::DirectoryPath>(path));
			directories.back()->adopt("entry");
		}
		paths.clear();
		directories.clear();
		for (const int descriptor : sockets)
			(void)close(descriptor);
	}
	std::error_code ignored;
	fs::remove_all(directory, ignored);
}

void require_window_reserved_and_empty()
{
	const auto window = uml::runtime::process_native_window();
	require(window.end > window.begin, "the process holds no window");
	for (std::uintptr_t cursor = window.begin; cursor < window.end;) {
		const auto region = uml::testing::region_containing(cursor);
		require(region.has_value() &&
			region->protection == VM_PROT_NONE,
			"the window holds a mapping or a gap after the hand-back");
		cursor = region->start + region->size;
	}
	require(nothing_present(reinterpret_cast<void *>(window.begin),
		window.end - window.begin),
		"a page of the window is present after the hand-back");
}

/* Called inside the run that boots again, when the hand-back has completed. */
void check_hand_back(void *, const GuestHandBackReport &report) noexcept
{
	++world.hand_backs;
	const auto &expected = world.expected;
	require(report.descriptors == expected.descriptors &&
		report.keys == expected.keys &&
		report.allocations == expected.allocations &&
		report.allocation_bytes == expected.allocation_bytes &&
		report.mappings == expected.mappings &&
		report.mapping_bytes == expected.mapping_bytes &&
		report.sbrk_calls == expected.sbrk_calls &&
		report.guest_files == expected.guest_files,
		"the hand-back did not give back what the kernel created");

	const auto counts = uml::runtime::kernel_resource_counts();
	require(counts.descriptors == 0 && counts.keys == 0 &&
		counts.allocations == 0 && counts.allocation_bytes == 0 &&
		counts.mappings == 0 && counts.mapping_bytes == 0 &&
		counts.sbrk_calls == 0, "the kernel-resource ledger is not empty");
	for (const int descriptor : world.descriptors)
		require(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF,
			"a descriptor the kernel left open survived the hand-back");
	require(fcntl(world.control, F_GETFD) != -1,
		"the hand-back closed a descriptor the kernel did not create");
	require(!uml::testing::region_containing(world.outside_mapping)
			.has_value(),
		"a mapping the kernel made outside the window survived");
	require(uml::runtime::reclaim_ended_kernel_threads() == 0,
		"a kernel-thread slot survived the hand-back");
	uml::runtime::KernelThreadSnapshot snapshot;
	uml::runtime::snapshot_kernel_threads(snapshot);
	require(!snapshot.overflowed && snapshot.creator_count == 0,
		"the kernel-thread table overflowed");

	require(uml::runtime::current_context() == nullptr,
		"a run route survived the hand-back");
	require(uml::platform::forget_guest_files() == 0,
		"a guest-file record survived the hand-back");
	require_socket_path_tables_empty();

	require(uml::memory::guest_ram_commitment() == nullptr,
		"the guest RAM commitment was not retired");
	require(!uml::memory::anonymous_shared_backing_registered() &&
		!uml::runtime::process_guest_spent(),
		"the guest RAM token was not unregistered");
	require_window_reserved_and_empty();
	require_same_process(world.before);

	malloc_statistics_t zone{};
	malloc_zone_statistics(malloc_default_zone(), &zone);
	world.zone_in_use.push_back(zone.size_in_use);
	world.zone_allocated = zone.size_allocated;
}

void require_hand_backs(int count)
{
	require(world.hand_backs == count,
		"the hand-back ran a different number of times");
}

struct HandBackHooks final {
	HandBackHooks()
	{
		uml::emulator::testing::set_guest_hand_back_hook(
			check_hand_back, nullptr);
	}
	~HandBackHooks()
	{
		uml::emulator::testing::set_guest_hand_back_hook(nullptr, nullptr);
	}
	HandBackHooks(const HandBackHooks &) = delete;
	HandBackHooks &operator=(const HandBackHooks &) = delete;
};

void begin_restarts(uml_emulator *handle, int restarting, Ending last,
	bool leave_thread = false)
{
	world.handle = handle;
	world.restarting = restarting;
	world.last = last;
	world.leave_thread = leave_thread;
	world.control = open("/dev/null", O_RDONLY);
	require(world.control >= 0, "could not open the control descriptor");
	world.before = capture_process_state();
}

[[nodiscard]] uml_emulator_run_end_kind run_end_kind(
	const uml_emulator *handle)
{
	uml_emulator_run_end end{.struct_size = sizeof(end)};
	require(uml_emulator_get_run_end(handle, &end) == UML_EMULATOR_OK,
		"the run end could not be read");
	return end.kind;
}

void restart_storm()
{
	constexpr int boots = 64;
	const HandBackHooks hooks;
	auto emulator = make_emulator(boot);
	begin_restarts(emulator.get(), boots - 1, Ending::power_off);
	for (int index = 1; index <= boots; ++index) {
		require(run(emulator.get()) == kernel_booted,
			"a boot of the restarting guest failed");
		require(kernel_entries == index,
			"a run did not enter its kernel once");
		require(run_end_kind(emulator.get()) == (index < boots ?
			UML_EMULATOR_RUN_END_RESTART :
			UML_EMULATOR_RUN_END_POWER_OFF),
			"a boot did not report how it ended");
		require_hand_backs(index - 1);
	}
	require_same_process(world.before);
	/* The last guest powered off: it stays spent. */
	require_refused(emulator.get());
	require(run_end_kind(emulator.get()) == UML_EMULATOR_RUN_END_NONE,
		"a refused run repeated the power-off");
	require_refused(make_emulator(boot).get());
	require_hand_backs(boots - 1);

	/* The default zone keeps what a boot freed (xzone malloc retains empty
	 * chunks); its in-use bytes, what the library or the kernel still holds,
	 * stay flat across hand-backs. */
	constexpr std::size_t slack = 1U << 20;
	const std::size_t baseline = world.zone_in_use[2];
	for (std::size_t index = 3; index < world.zone_in_use.size(); ++index)
		require(world.zone_in_use[index] <= baseline + slack,
			"the default zone's in-use bytes grew across hand-backs");
	std::fprintf(stderr,
		"default zone: %zu bytes in use after the 3rd hand-back, %zu after "
		"the %zu-th; %zu bytes allocated\n",
		baseline, world.zone_in_use.back(), world.zone_in_use.size(),
		world.zone_allocated);
	(void)close(world.control);
}

void a_spent_end_stays_spent(Ending ending,
	uml_emulator_run_end_kind expected)
{
	const HandBackHooks hooks;
	auto emulator = make_emulator(boot);
	begin_restarts(emulator.get(), 0, ending);
	require(run(emulator.get()) == kernel_booted,
		"the guest did not boot");
	require(run_end_kind(emulator.get()) == expected,
		"the run did not report how it ended");
	require_refused(emulator.get());
	require_refused(make_emulator(boot).get());
	require_hand_backs(0);
	(void)close(world.control);
}

void a_restart_belongs_to_its_handle()
{
	const HandBackHooks hooks;
	auto first = make_emulator(boot);
	begin_restarts(first.get(), 1, Ending::power_off);
	require(run(first.get()) == kernel_booted &&
		run_end_kind(first.get()) == UML_EMULATOR_RUN_END_RESTART,
		"the first boot did not restart");
	/* The process's guest waits for its own handle. */
	auto other = make_emulator(boot);
	require_refused(other.get());
	require_refused(other.get());
	require_hand_backs(0);
	require(run(first.get()) == kernel_booted &&
		run_end_kind(first.get()) == UML_EMULATOR_RUN_END_POWER_OFF,
		"the restarted guest did not boot on its own handle");
	require_hand_backs(1);
	require_refused(other.get());
	require_refused(first.get());
	(void)close(world.control);
}

void a_borrowed_view_stops_the_restart()
{
	const HandBackHooks hooks;
	auto emulator = make_emulator(boot);
	begin_restarts(emulator.get(), 1, Ending::power_off);
	require(run(emulator.get()) == kernel_booted,
		"the first boot failed");
	const std::size_t page = host_page();
	auto view = uml::memory::acquire_shared_backing_view(world.token,
		image_pages * page, page, PROT_READ | PROT_WRITE);
	require(static_cast<bool>(view), "could not borrow guest RAM");
	const auto *ram = static_cast<const std::uint8_t *>(view.data());
	const int entries = kernel_entries;
	const auto before = uml::runtime::kernel_resource_counts();
	require(before.descriptors != 0, "the kernel left no descriptors");
	for (int attempt = 0; attempt < 2; ++attempt) {
		require(run(emulator.get()) == -UML_EMULATOR_RUNTIME_ERROR,
			"a restart over a borrowed view did not fail");
		require(kernel_entries == entries,
			"a refused restart entered its kernel");
		require(run_end_kind(emulator.get()) == UML_EMULATOR_RUN_END_NONE,
			"a refused restart reported an end");
		/* Nothing was handed back. */
		const auto now = uml::runtime::kernel_resource_counts();
		require(now.descriptors == before.descriptors &&
			now.keys == before.keys &&
			now.allocations == before.allocations &&
			now.mappings == before.mappings,
			"a refused restart gave resources back");
		require(uml::memory::guest_ram_commitment() != nullptr &&
			uml::memory::anonymous_shared_backing_registered() &&
			ram[0] == 0xa1, "a refused restart took the guest's RAM");
	}
	require_hand_backs(0);
	{
		auto released = std::move(view);
	}
	require(run(emulator.get()) == kernel_booted,
		"the restart did not proceed once the view was released");
	require_hand_backs(1);
	require(kernel_entries == entries + 1, "the restart did not boot");
	(void)close(world.control);
}

void a_running_thread_stops_the_restart()
{
	const HandBackHooks hooks;
	auto emulator = make_emulator(boot);
	begin_restarts(emulator.get(), 1, Ending::power_off, true);
	require(run(emulator.get()) == kernel_booted && world.thread_started,
		"the first boot failed");
	const int entries = kernel_entries;
	const auto before = uml::runtime::kernel_resource_counts();
	require(before.descriptors != 0, "the kernel left no descriptors");
	require(run(emulator.get()) == -UML_EMULATOR_RUNTIME_ERROR,
		"a restart with a running kernel thread did not fail");
	require(kernel_entries == entries,
		"a refused restart entered its kernel");
	const auto now = uml::runtime::kernel_resource_counts();
	require(now.descriptors == before.descriptors,
		"a refused restart gave resources back");
	require_hand_backs(0);
	world.release_thread.store(true, std::memory_order_release);
	require(pthread_join(world.held_thread, nullptr) == 0,
		"could not join the held kernel thread");
	require(run(emulator.get()) == kernel_booted,
		"the restart did not proceed once the thread ended");
	require_hand_backs(1);
	(void)close(world.control);
}

} // namespace

int main(int argc, char **argv)
{
	try {
		const std::string_view mode = argc == 2 ? argv[1] : "";
		if (mode == "same-handle")
			second_run_on_the_same_handle();
		else if (mode == "fresh-handle")
			second_run_on_a_fresh_handle();
		else if (mode == "unclaimed-first-run")
			run_that_left_guest_ram();
		else if (mode == "restart")
			restart_storm();
		else if (mode == "power-off")
			a_spent_end_stays_spent(Ending::power_off,
				UML_EMULATOR_RUN_END_POWER_OFF);
		else if (mode == "halt")
			a_spent_end_stays_spent(Ending::halt,
				UML_EMULATOR_RUN_END_POWER_OFF);
		else if (mode == "stopped")
			a_spent_end_stays_spent(Ending::stopped,
				UML_EMULATOR_RUN_END_STOPPED);
		else if (mode == "failed")
			a_spent_end_stays_spent(Ending::failed,
				UML_EMULATOR_RUN_END_FAILED);
		else if (mode == "another-handle")
			a_restart_belongs_to_its_handle();
		else if (mode == "borrowed-view")
			a_borrowed_view_stops_the_restart();
		else if (mode == "running-thread")
			a_running_thread_stops_the_restart();
		else
			throw std::runtime_error{
				"usage: uml_runtime_sequential_run_tests "
				"same-handle|fresh-handle|unclaimed-first-run|restart|"
				"power-off|halt|stopped|failed|another-handle|"
				"borrowed-view|running-thread"};
	} catch (const std::exception &error) {
		std::fprintf(stderr, "%s\n", error.what());
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
