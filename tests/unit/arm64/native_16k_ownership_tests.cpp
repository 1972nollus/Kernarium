// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/emulator.h"
#include "allocation_fault.hpp"
#include "domain_registry_testing.hpp"
#include "native_retry_validation_test_support.hpp"
#include "native_window_test_support.hpp"
#include "scratch_backing.hpp"
#include "uml_darwin_abi.h"
#include "uml_darwin_memory.h"
#include "uml_darwin_static_mmu.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <memory>
#include <mutex>
#include <spawn.h>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace {

using namespace std::chrono_literals;

constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error{message};
}

/* Guest pages whose first bytes start as `page_values`. */
class TemporaryBacking final : public uml::testing::ScratchBacking {
public:
	explicit TemporaryBacking(
		std::initializer_list<std::uint8_t> page_values)
		: ScratchBacking{"uml-native-16k-owner-XXXXXX",
			  page_values.size() * page_size}
	{
		std::size_t page = 0;
		for (const auto value : page_values)
			put_byte(page++ * page_size, std::byte{value});
	}

	[[nodiscard]] std::uint8_t byte(std::size_t page = 0) const
	{
		return std::to_integer<std::uint8_t>(
			get_byte(page * page_size));
	}
};

class NativeWindowArena final {
public:
	explicit NativeWindowArena(std::size_t pages)
		: length_{pages * page_size}
	{
		base_ = uml::testing::reserve_native_test_window(length_);
		require(base_ != 0,
			"could not reserve native-window ownership arena");
		require(base_ >= uml::testing::native_test_window_start &&
			base_ <= uml::testing::native_test_window_end - length_ &&
			(base_ & (page_size - 1)) == 0,
			"native ownership arena escaped the shared native window");
		require(uml::testing::release_native_test_window(base_, length_),
			"could not release native-window ownership arena");
	}

	NativeWindowArena(const NativeWindowArena &) = delete;
	NativeWindowArena &operator=(const NativeWindowArena &) = delete;

	[[nodiscard]] std::uintptr_t page(std::size_t index) const noexcept
	{
		return base_ + index * page_size;
	}

private:
	std::uintptr_t base_{};
	std::size_t length_{};
};

/* The guest released this range, so the window's reservation holds it: nothing
 * is mapped and nothing else in the process can be given the address. */
[[nodiscard]] bool exactly_released(
	std::uintptr_t address, std::size_t length = page_size) noexcept
{
	return uml::testing::released_into_native_window(address, length);
}

/* Nothing has ever mapped this range, so it is still free address space. */
[[nodiscard]] bool exactly_absent(
	std::uintptr_t address, std::size_t length = page_size) noexcept
{
	mach_vm_address_t reservation = address;
	if (mach_vm_allocate(mach_task_self(), &reservation, length,
		    VM_FLAGS_FIXED) != KERN_SUCCESS)
		return false;
	return reservation == address &&
		mach_vm_deallocate(mach_task_self(), reservation, length) ==
			KERN_SUCCESS;
}

[[nodiscard]] bool counts_are_zero() noexcept
{
	return uml::memory::testing::native_domain_count() == 0 &&
		uml::memory::testing::native_host_mapping_count() == 0;
}

using KernelBody = int (*)(void *);

struct Dispatch final {
	KernelBody body{};
	void *context{};
	std::atomic_bool exception{};
};

Dispatch dispatch_a;
Dispatch dispatch_b;

void set_dispatch(Dispatch &dispatch, KernelBody body, void *context) noexcept
{
	dispatch.body = body;
	dispatch.context = context;
	dispatch.exception.store(false, std::memory_order_release);
}

int invoke(Dispatch &dispatch) noexcept
{
	try {
		if (dispatch.body == nullptr)
			return 120;
		return dispatch.body(dispatch.context);
	} catch (...) {
		dispatch.exception.store(true, std::memory_order_release);
		return 121;
	}
}

int kernel_a(int, char *const[], char *const[]) noexcept
{
	return invoke(dispatch_a);
}

int kernel_b(int, char *const[], char *const[]) noexcept
{
	return invoke(dispatch_b);
}

using Emulator = std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)>;

Emulator make_emulator(uml_emulator_kernel_entry entry, const char *rootfs)
{
	const uml_emulator_config config{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = rootfs,
		.native_kernel_entry = entry,
	};
	uml_emulator *created = nullptr;
	require(uml_emulator_create(&config, &created) == UML_EMULATOR_OK,
		"could not create native ownership emulator");
	return {created, uml_emulator_destroy};
}

int run(uml_emulator *emulator)
{
	const char *arguments[]{"native-16k-ownership"};
	return uml_emulator_run(emulator, 1, arguments, nullptr);
}

struct PairBarrier final {
	std::mutex mutex;
	std::condition_variable changed;
	std::size_t entered{};
	bool release{};
};

struct ConcurrentMapState final {
	PairBarrier *barrier{};
	std::uintptr_t address{};
	int fd{-1};
	std::uint8_t expected{};
	std::uint8_t replacement{};
	bool valid{};
};

int concurrent_map(void *opaque)
{
	auto &state = *static_cast<ConcurrentMapState *>(opaque);
	bool valid = darwin_uml_vm_map(reinterpret_cast<void *>(state.address),
		state.fd, 0, page_size, PROT_READ | PROT_WRITE, 0, 1) == 0;
	auto *byte = reinterpret_cast<volatile std::uint8_t *>(state.address);
	if (valid)
		valid = *byte == state.expected;
	if (valid)
		*byte = state.replacement;
	{
		std::unique_lock lock{state.barrier->mutex};
		++state.barrier->entered;
		state.barrier->changed.notify_all();
		valid = state.barrier->changed.wait_for(lock, 5s, [&] {
			return state.barrier->release;
		}) && valid;
	}
	if (valid)
		valid = *byte == state.replacement;
	state.valid = valid;
	return valid ? 0 : 70;
}

void test_nonoverlap_concurrency_and_leaked_cleanup(
	const NativeWindowArena &arena)
{
	TemporaryBacking backing_a{0x11};
	TemporaryBacking backing_b{0x22};
	PairBarrier barrier;
	ConcurrentMapState state_a{
		.barrier = &barrier,
		.address = arena.page(0),
		.fd = backing_a.descriptor(),
		.expected = 0x11,
		.replacement = 0x31,
	};
	ConcurrentMapState state_b{
		.barrier = &barrier,
		.address = arena.page(2),
		.fd = backing_b.descriptor(),
		.expected = 0x22,
		.replacement = 0x42,
	};
	set_dispatch(dispatch_a, concurrent_map, &state_a);
	set_dispatch(dispatch_b, concurrent_map, &state_b);
	auto handle_a = make_emulator(kernel_a, "/tmp/native-16k-concurrent-a.img");
	auto handle_b = make_emulator(kernel_b, "/tmp/native-16k-concurrent-b.img");
	int status_a = -1;
	int status_b = -1;
	std::thread first{[&] { status_a = run(handle_a.get()); }};
	std::thread second{[&] { status_b = run(handle_b.get()); }};
	bool overlapped = false;
	{
		std::unique_lock lock{barrier.mutex};
		overlapped = barrier.changed.wait_for(lock, 5s, [&] {
			return barrier.entered == 2;
		});
		barrier.release = true;
	}
	barrier.changed.notify_all();
	first.join();
	second.join();
	require(overlapped && status_a == 0 && status_b == 0 &&
		state_a.valid && state_b.valid &&
		!dispatch_a.exception.load(std::memory_order_acquire) &&
		!dispatch_b.exception.load(std::memory_order_acquire),
		"distinct native ownership domains did not overlap safely");
	require(backing_a.byte() == 0x31 && backing_b.byte() == 0x42,
		"callback-leaked mappings were not flushed to their own backings");
	require(exactly_released(arena.page(0)) &&
		exactly_released(arena.page(2)),
		"callback-leaked mapping survived domain retirement");
	require(counts_are_zero(),
		"nonoverlap retirement retained a native domain or host owner");
}

struct HoldState final {
	std::mutex mutex;
	std::condition_variable changed;
	std::uintptr_t address{};
	int fd{-1};
	std::uint8_t expected{};
	std::uint8_t replacement{};
	bool entered{};
	bool release{};
	bool valid{};
};

int hold_map(void *opaque)
{
	auto &state = *static_cast<HoldState *>(opaque);
	bool valid = darwin_uml_vm_map(reinterpret_cast<void *>(state.address),
		state.fd, 0, page_size, PROT_READ | PROT_WRITE, 0, 1) == 0;
	auto *byte = reinterpret_cast<volatile std::uint8_t *>(state.address);
	if (valid)
		valid = *byte == state.expected;
	if (valid)
		*byte = state.replacement;
	{
		std::unique_lock lock{state.mutex};
		state.entered = true;
		state.changed.notify_all();
		valid = state.changed.wait_for(lock, 5s, [&] {
			return state.release;
		}) && valid;
	}
	if (valid)
		valid = *byte == state.replacement;
	state.valid = valid;
	return valid ? 0 : 71;
}

bool wait_until_entered(HoldState &state)
{
	std::unique_lock lock{state.mutex};
	return state.changed.wait_for(lock, 5s, [&] { return state.entered; });
}

void release(HoldState &state)
{
	{
		std::lock_guard lock{state.mutex};
		state.release = true;
	}
	state.changed.notify_all();
}

struct CollisionState final {
	std::mutex mutex;
	std::condition_variable changed;
	std::uintptr_t address{};
	int fd{-1};
	bool map_failed{};
	bool protect_failed{};
	bool unmap_failed{};
	bool drop_failed{};
	bool sync_failed{};
	bool retry_failed{};
	bool finished{};
};

int collide_with_foreign_owner(void *opaque)
{
	auto &state = *static_cast<CollisionState *>(opaque);
	errno = 0;
	const int map_result = darwin_uml_vm_map(
		reinterpret_cast<void *>(state.address), state.fd, 0, page_size,
		PROT_READ | PROT_WRITE, 0, 1);
	state.map_failed = map_result == -1 && errno == EBUSY;
	errno = 0;
	const int protect_result = darwin_uml_vm_protect(
		reinterpret_cast<void *>(state.address), page_size, PROT_NONE);
	state.protect_failed = protect_result == -1 && errno == EBUSY;
	errno = 0;
	const int unmap_result = darwin_uml_vm_unmap(
		reinterpret_cast<void *>(state.address), page_size);
	state.unmap_failed = unmap_result == -1 && errno == EBUSY;
	errno = 0;
	const int drop_result = darwin_uml_vm_drop(
		reinterpret_cast<void *>(state.address), page_size);
	state.drop_failed = drop_result == -1 && errno == EBUSY;
	errno = 0;
	const int sync_result = darwin_uml_vm_sync_from_backing(state.address);
	state.sync_failed = sync_result == -1 && errno == EBUSY;
	errno = 0;
	const int retry_result = darwin_uml_vm_retry_fault(
		state.address, 0, 1);
	state.retry_failed = retry_result == -1 && errno == EBUSY;
	{
		std::lock_guard lock{state.mutex};
		state.finished = true;
	}
	state.changed.notify_all();
	return state.map_failed && state.protect_failed && state.unmap_failed &&
		state.drop_failed && state.sync_failed && state.retry_failed ? 0 : 72;
}

struct SimpleMapState final {
	std::uintptr_t address{};
	int fd{-1};
	std::uint8_t expected{};
	std::uint8_t replacement{};
	bool valid{};
};

int simple_map(void *opaque)
{
	auto &state = *static_cast<SimpleMapState *>(opaque);
	state.valid = darwin_uml_vm_map(
		reinterpret_cast<void *>(state.address), state.fd, 0, page_size,
		PROT_READ | PROT_WRITE, 0, 1) == 0;
	if (state.valid) {
		auto *byte = reinterpret_cast<volatile std::uint8_t *>(state.address);
		state.valid = *byte == state.expected;
		if (state.valid)
			*byte = state.replacement;
	}
	return state.valid ? 0 : 73;
}

void test_same_address_collision_and_retry(const NativeWindowArena &arena)
{
	TemporaryBacking backing_a{0x15};
	TemporaryBacking backing_b{0x26};
	HoldState holder{
		.address = arena.page(4),
		.fd = backing_a.descriptor(),
		.expected = 0x15,
		.replacement = 0x35,
	};
	CollisionState collision{
		.address = arena.page(4),
		.fd = backing_b.descriptor(),
	};
	set_dispatch(dispatch_a, hold_map, &holder);
	set_dispatch(dispatch_b, collide_with_foreign_owner, &collision);
	auto handle_a = make_emulator(kernel_a, "/tmp/native-16k-collision-a.img");
	auto handle_b = make_emulator(kernel_b, "/tmp/native-16k-collision-b.img");
	int status_a = -1;
	int status_b = -1;
	std::thread first{[&] { status_a = run(handle_a.get()); }};
	require(wait_until_entered(holder),
		"same-address ownership holder did not enter");
	std::thread second{[&] { status_b = run(handle_b.get()); }};
	bool prompt = false;
	{
		std::unique_lock lock{collision.mutex};
		prompt = collision.changed.wait_for(lock, 2s, [&] {
			return collision.finished;
		});
	}
	release(holder);
	first.join();
	second.join();
	require(prompt && status_a == 0 && status_b == 0 && holder.valid &&
		collision.map_failed && collision.protect_failed &&
		collision.unmap_failed && collision.drop_failed &&
		collision.sync_failed && collision.retry_failed,
		"foreign same-address operations did not fail promptly and atomically");
	require(backing_a.byte() == 0x35 && backing_b.byte() == 0x26,
		"same-address collision altered the holder or contender backing");
	require(exactly_released(arena.page(4)) && counts_are_zero(),
		"same-address holder did not retire completely");

	SimpleMapState retry{
		.address = arena.page(4),
		.fd = backing_b.descriptor(),
		.expected = 0x26,
		.replacement = 0x46,
	};
	set_dispatch(dispatch_b, simple_map, &retry);
	require(run(handle_b.get()) == 0 && retry.valid,
		"same-address contender could not retry after owner retirement");
	require(backing_b.byte() == 0x46 && exactly_released(arena.page(4)) &&
		counts_are_zero(),
		"same-address retry did not flush and retire cleanly");
}

struct AcquireRaceState final {
	std::mutex mutex;
	std::condition_variable changed;
	std::uintptr_t address{};
	int fd{-1};
	std::uint8_t expected{};
	std::uint8_t replacement{};
	bool saw_busy{};
	bool acquired{};
	bool release{};
	bool valid{};
};

int acquire_after_retirement(void *opaque)
{
	auto &state = *static_cast<AcquireRaceState *>(opaque);
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	bool valid = true;
	bool acquired = false;
	while (std::chrono::steady_clock::now() < deadline) {
		errno = 0;
		if (darwin_uml_vm_map(reinterpret_cast<void *>(state.address),
			state.fd, 0, page_size, PROT_READ | PROT_WRITE, 0, 1) == 0) {
			{
				std::lock_guard lock{state.mutex};
				state.acquired = true;
			}
			state.changed.notify_all();
			acquired = true;
			break;
		}
		if (errno != EBUSY) {
			valid = false;
			break;
		}
		{
			std::lock_guard lock{state.mutex};
			state.saw_busy = true;
		}
		state.changed.notify_all();
		std::this_thread::yield();
	}
	if (!acquired)
		valid = false;
	if (valid) {
		auto *byte = reinterpret_cast<volatile std::uint8_t *>(state.address);
		valid = *byte == state.expected;
		if (valid)
			*byte = state.replacement;
	}
	{
		std::unique_lock lock{state.mutex};
		state.changed.notify_all();
		valid = state.changed.wait_for(lock, 5s, [&] {
			return state.release;
		}) && valid;
	}
	if (valid)
		valid = *reinterpret_cast<volatile std::uint8_t *>(state.address) ==
			state.replacement;
	state.valid = valid;
	return valid ? 0 : 74;
}

void test_retire_acquire_handoff(const NativeWindowArena &arena)
{
	TemporaryBacking backing_a{0x18};
	TemporaryBacking backing_b{0x29};
	HoldState holder{
		.address = arena.page(6),
		.fd = backing_a.descriptor(),
		.expected = 0x18,
		.replacement = 0x38,
	};
	AcquireRaceState contender{
		.address = arena.page(6),
		.fd = backing_b.descriptor(),
		.expected = 0x29,
		.replacement = 0x49,
	};
	set_dispatch(dispatch_a, hold_map, &holder);
	set_dispatch(dispatch_b, acquire_after_retirement, &contender);
	auto handle_a = make_emulator(kernel_a, "/tmp/native-16k-handoff-a.img");
	auto handle_b = make_emulator(kernel_b, "/tmp/native-16k-handoff-b.img");
	int status_a = -1;
	int status_b = -1;
	std::thread first{[&] { status_a = run(handle_a.get()); }};
	require(wait_until_entered(holder),
		"retire/acquire holder did not enter");
	std::thread second{[&] { status_b = run(handle_b.get()); }};
	{
		std::unique_lock lock{contender.mutex};
		require(contender.changed.wait_for(lock, 2s, [&] {
			return contender.saw_busy;
		}), "retire/acquire contender did not observe the live owner");
	}
	release(holder);
	first.join();
	{
		std::unique_lock lock{contender.mutex};
		require(contender.changed.wait_for(lock, 5s, [&] {
			return contender.acquired;
		}), "retire/acquire contender did not acquire the retired address");
		contender.release = true;
	}
	contender.changed.notify_all();
	second.join();
	require(status_a == 0 && status_b == 0 && holder.valid &&
		contender.saw_busy && contender.acquired && contender.valid,
		"retire/acquire handoff lost or unmapped the successor mapping");
	require(backing_a.byte() == 0x38 && backing_b.byte() == 0x49 &&
		exactly_released(arena.page(6)) && counts_are_zero(),
		"retire/acquire handoff did not flush and retire both owners");
}

struct ReplacementState final {
	std::uintptr_t address{};
	int first_fd{-1};
	int second_fd{-1};
	bool valid{};
};

int replace_same_owner(void *opaque)
{
	auto &state = *static_cast<ReplacementState *>(opaque);
	bool valid = darwin_uml_vm_map(reinterpret_cast<void *>(state.address),
		state.first_fd, 0, page_size, PROT_READ | PROT_WRITE, 0, 1) == 0;
	if (valid)
		valid = *reinterpret_cast<volatile std::uint8_t *>(state.address) ==
			0x51;
	if (valid)
		valid = darwin_uml_vm_map(reinterpret_cast<void *>(state.address),
			state.second_fd, 0, page_size, PROT_READ | PROT_WRITE, 0, 1) == 0;
	if (valid)
		valid = *reinterpret_cast<volatile std::uint8_t *>(state.address) ==
			0x62;
	if (valid)
		*reinterpret_cast<volatile std::uint8_t *>(state.address) = 0x72;
	state.valid = valid;
	return valid ? 0 : 75;
}

void test_same_owner_replacement(const NativeWindowArena &arena)
{
	TemporaryBacking first{0x51};
	TemporaryBacking second{0x62};
	ReplacementState state{
		.address = arena.page(8),
		.first_fd = first.descriptor(),
		.second_fd = second.descriptor(),
	};
	set_dispatch(dispatch_a, replace_same_owner, &state);
	auto handle = make_emulator(kernel_a, "/tmp/native-16k-replace.img");
	require(run(handle.get()) == 0 && state.valid,
		"same owner could not replace its direct mapping");
	require(first.byte() == 0x51 && second.byte() == 0x72 &&
		exactly_released(arena.page(8)) && counts_are_zero(),
		"same-owner replacement crossed backings or survived retirement");
}

struct MixedState final {
	std::mutex mutex;
	std::condition_variable changed;
	std::uintptr_t address{};
	int fd{-1};
	bool map_failed{};
	bool protect_failed{};
	bool unmap_failed{};
	bool prefix_absent{};
	bool finished{};
};

int collide_mixed_range(void *opaque)
{
	auto &state = *static_cast<MixedState *>(opaque);
	state.map_failed = darwin_uml_vm_map(
		reinterpret_cast<void *>(state.address), state.fd, 0, 2 * page_size,
		PROT_READ | PROT_WRITE, 0, 1) == -1;
	state.protect_failed = darwin_uml_vm_protect(
		reinterpret_cast<void *>(state.address), 2 * page_size,
		PROT_READ) == -1;
	state.unmap_failed = darwin_uml_vm_unmap(
		reinterpret_cast<void *>(state.address), 2 * page_size) == -1;
	state.prefix_absent = exactly_absent(state.address);
	{
		std::lock_guard lock{state.mutex};
		state.finished = true;
	}
	state.changed.notify_all();
	return state.map_failed && state.protect_failed && state.unmap_failed &&
		state.prefix_absent ? 0 : 76;
}

struct TwoPageState final {
	std::uintptr_t address{};
	int fd{-1};
	bool valid{};
};

int map_two_pages(void *opaque)
{
	auto &state = *static_cast<TwoPageState *>(opaque);
	state.valid = darwin_uml_vm_map(
		reinterpret_cast<void *>(state.address), state.fd, 0, 2 * page_size,
		PROT_READ | PROT_WRITE, 0, 1) == 0;
	if (state.valid) {
		auto *first = reinterpret_cast<volatile std::uint8_t *>(state.address);
		auto *second = reinterpret_cast<volatile std::uint8_t *>(
			state.address + page_size);
		state.valid = *first == 0x83 && *second == 0x94;
		if (state.valid) {
			*first = 0xa3;
			*second = 0xb4;
		}
	}
	return state.valid ? 0 : 77;
}

void test_mixed_foreign_range_is_atomic(const NativeWindowArena &arena)
{
	TemporaryBacking holder_backing{0x47};
	TemporaryBacking mixed_backing{0x83, 0x94};
	HoldState holder{
		.address = arena.page(11),
		.fd = holder_backing.descriptor(),
		.expected = 0x47,
		.replacement = 0x57,
	};
	MixedState mixed{
		.address = arena.page(10),
		.fd = mixed_backing.descriptor(),
	};
	set_dispatch(dispatch_a, hold_map, &holder);
	set_dispatch(dispatch_b, collide_mixed_range, &mixed);
	auto handle_a = make_emulator(kernel_a, "/tmp/native-16k-mixed-a.img");
	auto handle_b = make_emulator(kernel_b, "/tmp/native-16k-mixed-b.img");
	int status_a = -1;
	int collision_status = -1;
	std::thread first{[&] { status_a = run(handle_a.get()); }};
	require(wait_until_entered(holder), "mixed-range holder did not enter");
	std::thread second{[&] { collision_status = run(handle_b.get()); }};
	bool prompt = false;
	{
		std::unique_lock lock{mixed.mutex};
		prompt = mixed.changed.wait_for(lock, 2s, [&] {
			return mixed.finished;
		});
	}
	release(holder);
	first.join();
	second.join();
	require(prompt && collision_status == 0 && mixed.map_failed &&
		mixed.protect_failed && mixed.unmap_failed && mixed.prefix_absent &&
		counts_are_zero(),
		"mixed foreign range partially mutated ownership");
	require(status_a == 0 && holder.valid && holder_backing.byte() == 0x57 &&
		counts_are_zero(), "mixed-range holder was altered by contender");

	TwoPageState retry{
		.address = arena.page(10),
		.fd = mixed_backing.descriptor(),
	};
	set_dispatch(dispatch_b, map_two_pages, &retry);
	require(run(handle_b.get()) == 0 && retry.valid,
		"mixed range could not be acquired after foreign retirement");
	require(mixed_backing.byte(0) == 0xa3 &&
		mixed_backing.byte(1) == 0xb4 &&
		exactly_released(arena.page(10), 2 * page_size) && counts_are_zero(),
		"mixed-range retry did not flush and retire atomically");
}

struct UnownedState final {
	std::uintptr_t reserved_address{};
	std::uintptr_t absent_address{};
	int fd{-1};
	bool valid{};
};

int test_unowned_operations(void *opaque)
{
	auto &state = *static_cast<UnownedState *>(opaque);
	mach_vm_address_t reservation = state.reserved_address;
	bool valid = mach_vm_allocate(mach_task_self(), &reservation, page_size,
		VM_FLAGS_FIXED) == KERN_SUCCESS &&
		reservation == state.reserved_address;
	constexpr std::uint8_t marker = 0x6d;
	if (valid)
		*reinterpret_cast<volatile std::uint8_t *>(state.reserved_address) =
			marker;
	if (valid)
		valid = darwin_uml_vm_map(
			reinterpret_cast<void *>(state.reserved_address), state.fd, 0,
			page_size, PROT_READ | PROT_WRITE, 0, 1) == 0;
	if (valid)
		valid = *reinterpret_cast<volatile std::uint8_t *>(
			state.reserved_address) == 0xc5;
	if (valid)
		valid = darwin_uml_vm_protect(
			reinterpret_cast<void *>(state.reserved_address), page_size,
			PROT_READ | PROT_WRITE) == 0;
	if (valid)
		valid = darwin_uml_vm_unmap(
			reinterpret_cast<void *>(state.reserved_address), page_size) == 0;
	if (valid)
		valid = exactly_released(state.reserved_address);
	if (valid)
		valid = darwin_uml_vm_protect(
			reinterpret_cast<void *>(state.absent_address), page_size,
			PROT_READ) == -1;
	if (valid)
		valid = darwin_uml_vm_unmap(
			reinterpret_cast<void *>(state.absent_address), page_size) == 0;
	if (valid)
		valid = exactly_absent(state.absent_address) &&
			uml::memory::testing::native_host_mapping_count() == 0;
	state.valid = valid;
	return valid ? 0 : 78;
}

void test_unowned_host_mapping_is_replaced(const NativeWindowArena &arena)
{
	TemporaryBacking backing{0xc5};
	UnownedState state{
		.reserved_address = arena.page(14),
		.absent_address = arena.page(15),
		.fd = backing.descriptor(),
	};
	set_dispatch(dispatch_a, test_unowned_operations, &state);
	auto handle = make_emulator(kernel_a, "/tmp/native-16k-unowned.img");
	require(run(handle.get()) == 0 && state.valid &&
		exactly_released(arena.page(14)) && counts_are_zero(),
		"unowned host mapping did not follow required MAP_FIXED replacement");
}

struct FailureRetryState final {
	std::uintptr_t address{};
	int fd{-1};
	uml::memory::testing::AllocationSite site{
		uml::memory::testing::AllocationSite::none};
	bool inject{};
	bool valid{};
};

int fail_then_retry_map(void *opaque)
{
	auto &state = *static_cast<FailureRetryState *>(opaque);
	if (state.inject)
		uml::memory::testing::fail_next_allocation(state.site);
	errno = 0;
	const int failed = darwin_uml_vm_map(
		reinterpret_cast<void *>(state.address), state.inject ? state.fd : -1,
		0, page_size, PROT_READ | PROT_WRITE, 0, 1);
	bool valid = failed == -1 &&
		(!state.inject || errno == ENOMEM) && exactly_absent(state.address) &&
		uml::memory::testing::native_host_mapping_count() == 0;
	if (valid)
		valid = darwin_uml_vm_map(
			reinterpret_cast<void *>(state.address), state.fd, 0, page_size,
			PROT_READ | PROT_WRITE, 0, 1) == 0;
	if (valid) {
		auto *byte = reinterpret_cast<volatile std::uint8_t *>(state.address);
		valid = *byte == 0xd6;
		if (valid)
			*byte = 0xe6;
	}
	state.valid = valid;
	return valid ? 0 : 79;
}

void run_failure_retry(const NativeWindowArena &arena, std::size_t page,
	uml::memory::testing::AllocationSite site, bool inject)
{
	TemporaryBacking backing{0xd6};
	FailureRetryState state{
		.address = arena.page(page),
		.fd = backing.descriptor(),
		.site = site,
		.inject = inject,
	};
	set_dispatch(dispatch_a, fail_then_retry_map, &state);
	auto handle = make_emulator(kernel_a, "/tmp/native-16k-rollback.img");
	require(run(handle.get()) == 0 && state.valid && backing.byte() == 0xe6 &&
		exactly_released(state.address) && counts_are_zero(),
		"failed native map did not roll back completely and permit retry");
}

struct OwnedLifecycleState final {
	std::uintptr_t address{};
	int fd{-1};
	bool valid{};
};

struct DirectSoftwarePublicationState final {
	std::uintptr_t native_address{};
	int fd{-1};
	bool valid{};
};

int direct_software_publication(void *opaque)
{
	auto &state = *static_cast<DirectSoftwarePublicationState *>(opaque);
	constexpr unsigned long generation = 0x22;
	constexpr std::uintptr_t guest_address = 0x50'0000;
	bool valid = darwin_uml_vm_map(
		reinterpret_cast<void *>(state.native_address), state.fd, 0, page_size,
		PROT_READ | PROT_WRITE, 0, 1) == 0;
	if (valid)
		*reinterpret_cast<volatile std::uint8_t *>(state.native_address) = 0x91;
	if (valid)
		valid = darwin_uml_vm_map_generation(generation,
			reinterpret_cast<void *>(guest_address), state.fd, 0, page_size,
			PROT_READ | PROT_EXEC, 1, 0) == 0;
	void *translated = nullptr;
	if (valid)
		valid = darwin_uml_static_vm_translate(generation, guest_address,
			DARWIN_UML_STATIC_EXECUTE, &translated) ==
			DARWIN_UML_STATIC_TRANSLATED && translated != nullptr &&
			*static_cast<const std::uint8_t *>(translated) == 0x91;
	const std::uint8_t replacement = 0xa2;
	if (valid)
		valid = pwrite(state.fd, &replacement, sizeof(replacement), 0) ==
			static_cast<ssize_t>(sizeof(replacement)) &&
			darwin_uml_vm_sync_from_backing(state.native_address) == 0 &&
			*reinterpret_cast<volatile std::uint8_t *>(
				state.native_address) == replacement;
	const bool low_cleanup = darwin_uml_vm_unmap_generation(generation,
		reinterpret_cast<void *>(guest_address), page_size) == 0;
	const bool generation_cleanup = darwin_uml_vm_destroy(generation) == 0;
	const bool native_cleanup = darwin_uml_vm_unmap(
		reinterpret_cast<void *>(state.native_address), page_size) == 0;
	state.valid = valid && low_cleanup && generation_cleanup && native_cleanup;
	return state.valid ? 0 : 82;
}

void test_direct_software_publication_is_identity(
	const NativeWindowArena &arena)
{
	TemporaryBacking backing{0x12};
	DirectSoftwarePublicationState state{
		.native_address = arena.page(23),
		.fd = backing.descriptor(),
	};
	set_dispatch(dispatch_a, direct_software_publication, &state);
	auto handle = make_emulator(
		kernel_a, "/tmp/native-16k-software-publication.img");
	require(run(handle.get()) == 0 && state.valid && backing.byte() == 0xa2 &&
		exactly_released(state.native_address) && counts_are_zero(),
		"16 KiB shared mapping was not an identity publication path");
}

[[nodiscard]] bool backing_byte_equals(
	int fd, std::size_t page, std::uint8_t expected) noexcept
{
	std::uint8_t actual{};
	ssize_t result;
	do {
		result = pread(fd, &actual, sizeof(actual),
			static_cast<off_t>(page * page_size));
	} while (result < 0 && errno == EINTR);
	return result == static_cast<ssize_t>(sizeof(actual)) &&
		actual == expected;
}

int owned_bridge_lifecycle(void *opaque)
{
	auto &state = *static_cast<OwnedLifecycleState *>(opaque);
	bool valid = darwin_uml_vm_map_generation(17,
		reinterpret_cast<void *>(state.address), state.fd, 0,
		2 * page_size, PROT_READ | PROT_WRITE, 0, 1) == 0;
	auto *first = reinterpret_cast<volatile std::uint8_t *>(state.address);
	auto *second = reinterpret_cast<volatile std::uint8_t *>(
		state.address + page_size);
	if (valid)
		valid = *first == 0x31 && *second == 0x42;
	if (valid) {
		*first = 0x51;
		*second = 0x62;
	}
	if (valid)
		valid = darwin_uml_vm_protect(
			reinterpret_cast<void *>(state.address), 2 * page_size,
			PROT_READ) == 0;
	if (valid)
		valid = *first == 0x51 && *second == 0x62;
	if (valid)
		valid = darwin_uml_vm_protect(
			reinterpret_cast<void *>(state.address), 2 * page_size,
			PROT_READ | PROT_WRITE) == 0;
	if (valid)
		valid = darwin_uml_vm_drop(
			reinterpret_cast<void *>(state.address), 2 * page_size) == 0;
	if (valid)
		valid = backing_byte_equals(state.fd, 0, 0x51) &&
			backing_byte_equals(state.fd, 1, 0x62);
	if (valid)
		valid = darwin_uml_vm_sync_from_backing(state.address) == 0 &&
			darwin_uml_vm_sync_from_backing(
				state.address + page_size) == 0;
	if (valid)
		valid = darwin_uml_vm_retry_fault(state.address, 0, 0) == 1 &&
			darwin_uml_vm_retry_fault(state.address, 0, 1) == 1 &&
			darwin_uml_vm_retry_fault(state.address, 1, 0) == 0 &&
			darwin_uml_vm_retry_fault(state.address, 1, 1) == 0;
	if (valid)
		valid = darwin_uml_vm_unmap_generation(17,
			reinterpret_cast<void *>(state.address), page_size) == 0;
	if (valid)
		valid = exactly_released(state.address) &&
			uml::memory::testing::native_host_mapping_count() == 1;
	if (valid)
		valid = darwin_uml_vm_unmap(
			reinterpret_cast<void *>(state.address + page_size),
			page_size) == 0;
	if (valid)
		valid = exactly_released(state.address, 2 * page_size) &&
			uml::memory::testing::native_host_mapping_count() == 0;
	if (valid)
		valid = darwin_uml_vm_map(
			reinterpret_cast<void *>(state.address), state.fd, 0,
			2 * page_size, PROT_READ | PROT_WRITE, 0, 1) == 0;
	if (valid)
		valid = *first == 0x51 && *second == 0x62;
	if (valid)
		valid = darwin_uml_vm_unmap(
			reinterpret_cast<void *>(state.address), 2 * page_size) == 0;
	state.valid = valid &&
		exactly_released(state.address, 2 * page_size) &&
		uml::memory::testing::native_host_mapping_count() == 0;
	return state.valid ? 0 : 81;
}

void test_owned_bridge_lifecycle(const NativeWindowArena &arena)
{
	TemporaryBacking backing{0x31, 0x42};
	OwnedLifecycleState state{
		.address = arena.page(21),
		.fd = backing.descriptor(),
	};
	set_dispatch(dispatch_a, owned_bridge_lifecycle, &state);
	auto handle = make_emulator(kernel_a, "/tmp/native-16k-lifecycle.img");
	require(run(handle.get()) == 0 && state.valid &&
		backing.byte(0) == 0x51 && backing.byte(1) == 0x62 &&
		counts_are_zero(),
		"owned native bridge lifecycle did not complete atomically");
}

void test_direct_retry_validates_ownership_mapping_and_permissions(
	const NativeWindowArena &arena)
{
	TemporaryBacking backing{0x93};
	uml::testing::exercise_native_retry_direct(arena.page(24),
		arena.page(25), arena.page(26), page_size, backing.descriptor(), require);
}

struct TwoPageRollbackState final {
	std::uintptr_t address{};
	int fd{-1};
	bool valid{};
};

int two_page_rollback(void *opaque)
{
	auto &state = *static_cast<TwoPageRollbackState *>(opaque);
	const auto absent_and_unowned = [&]() {
		return exactly_absent(state.address, 2 * page_size) &&
			uml::memory::testing::native_host_mapping_count() == 0;
	};
	const auto released_and_unowned = [&]() {
		return exactly_released(state.address, 2 * page_size) &&
			uml::memory::testing::native_host_mapping_count() == 0;
	};
	const auto injected_failure = [&](
		uml::memory::testing::AllocationSite site,
		std::size_t occurrence) {
		uml::memory::testing::fail_allocation_at(site, occurrence);
		errno = 0;
		const int result = darwin_uml_vm_map(
			reinterpret_cast<void *>(state.address), state.fd, 0,
			2 * page_size, PROT_READ | PROT_WRITE, 0, 1);
		const bool failed = result == -1 && errno == ENOMEM;
		uml::memory::testing::clear_allocation_failure();
		if (result == 0)
			(void)darwin_uml_vm_unmap(
				reinterpret_cast<void *>(state.address), 2 * page_size);
		return failed && absent_and_unowned();
	};

	bool valid = injected_failure(
		uml::memory::testing::AllocationSite::native_direct_record, 2);
	if (valid)
		valid = injected_failure(
			uml::memory::testing::AllocationSite::native_owner_record, 2);
	if (valid)
		valid = injected_failure(
			uml::memory::testing::AllocationSite::native_after_mmap, 1);
	if (valid) {
		errno = 0;
		const int result = darwin_uml_vm_map(
			reinterpret_cast<void *>(state.address), -1, 0,
			2 * page_size, PROT_READ | PROT_WRITE, 0, 1);
		valid = result == -1 && absent_and_unowned();
		if (result == 0)
			(void)darwin_uml_vm_unmap(
				reinterpret_cast<void *>(state.address), 2 * page_size);
	}
	if (valid) {
		mach_vm_address_t reservation = state.address + page_size;
		const bool reserved = mach_vm_allocate(mach_task_self(), &reservation,
			page_size, VM_FLAGS_FIXED) == KERN_SUCCESS &&
			reservation == state.address + page_size;
		if (reserved)
			*reinterpret_cast<volatile std::uint8_t *>(reservation) = 0x7a;
		errno = 0;
		const int result = reserved ? darwin_uml_vm_map(
			reinterpret_cast<void *>(state.address), -1, 0,
			2 * page_size, PROT_READ | PROT_WRITE, 0, 1) : -1;
		valid = reserved && result == -1 &&
			exactly_absent(state.address) &&
			*reinterpret_cast<volatile std::uint8_t *>(reservation) == 0x7a &&
			uml::memory::testing::native_host_mapping_count() == 0;
		if (reserved)
			valid = mach_vm_deallocate(mach_task_self(), reservation,
				page_size) == KERN_SUCCESS && valid;
	}
	if (valid)
		valid = absent_and_unowned() && darwin_uml_vm_map(
			reinterpret_cast<void *>(state.address), state.fd, 0,
			2 * page_size, PROT_READ | PROT_WRITE, 0, 1) == 0;
	if (valid) {
		auto *first = reinterpret_cast<volatile std::uint8_t *>(state.address);
		auto *second = reinterpret_cast<volatile std::uint8_t *>(
			state.address + page_size);
		valid = *first == 0x73 && *second == 0x84;
	}
	if (valid)
		valid = darwin_uml_vm_unmap(
			reinterpret_cast<void *>(state.address), 2 * page_size) == 0;
	state.valid = valid && released_and_unowned();
	return state.valid ? 0 : 82;
}

void test_two_page_rollback(const NativeWindowArena &arena)
{
	TemporaryBacking backing{0x73, 0x84};
	TwoPageRollbackState state{
		/* A rollback leaves the range exactly as it found it, so this
		 * test needs a range nothing has mapped: a released range is a
		 * window reservation, not free address space. */
		.address = arena.page(12),
		.fd = backing.descriptor(),
	};
	set_dispatch(dispatch_a, two_page_rollback, &state);
	auto handle = make_emulator(kernel_a, "/tmp/native-16k-two-page.img");
	require(run(handle.get()) == 0 && state.valid && counts_are_zero(),
		"two-page native map rollback retained state or blocked retry");
}

struct RegistryOomState final {
	std::uintptr_t address{};
	int fd{-1};
	uml::memory::testing::AllocationSite site{
		uml::memory::testing::AllocationSite::none};
	bool safe{};
};

int registry_oom(void *opaque)
{
	auto &state = *static_cast<RegistryOomState *>(opaque);
	uml::memory::testing::fail_next_allocation(state.site);
	errno = 0;
	const int failed_result = darwin_uml_vm_map(
		reinterpret_cast<void *>(state.address), state.fd, 0, page_size,
		PROT_READ | PROT_WRITE, 0, 1);
	const bool failure_was_contained = failed_result == -1 &&
		(errno == ENOMEM || errno == ESRCH) && exactly_absent(state.address);
	const int retry_result = failure_was_contained ? darwin_uml_vm_map(
		reinterpret_cast<void *>(state.address), state.fd, 0, page_size,
		PROT_READ | PROT_WRITE, 0, 1) : -1;
	const bool retry_succeeded = retry_result == 0 &&
		*reinterpret_cast<volatile std::uint8_t *>(state.address) == 0xf7;
	const int unmap_result = retry_succeeded ? darwin_uml_vm_unmap(
		reinterpret_cast<void *>(state.address), page_size) : -1;
	state.safe = failure_was_contained && retry_succeeded &&
		unmap_result == 0 && exactly_released(state.address);
	return state.safe ? 0 : 80;
}

int run_registry_oom_child(std::string_view mode)
{
	NativeWindowArena arena{2};
	TemporaryBacking backing{0xf7};
	const auto site = mode == "--native-domain-registry-oom-child" ?
		uml::memory::testing::AllocationSite::native_domain_registry :
		uml::memory::testing::AllocationSite::native_owner_registry;
	RegistryOomState state{
		.address = arena.page(0),
		.fd = backing.descriptor(),
		.site = site,
	};
	set_dispatch(dispatch_a, registry_oom, &state);
	auto handle = make_emulator(kernel_a, "/tmp/native-16k-registry-oom.img");
	const int status = run(handle.get());
	return status == 0 && state.safe &&
		uml::memory::testing::native_domain_count() == 0 &&
		exactly_released(state.address) ? EXIT_SUCCESS : EXIT_FAILURE;
}

void require_registry_oom_child(const char *executable, const char *mode)
{
	pid_t child = -1;
	char *arguments[]{const_cast<char *>(executable),
		const_cast<char *>(mode), nullptr};
	require(posix_spawnp(&child, executable, nullptr, nullptr, arguments,
		environ) == 0, "could not spawn native registry OOM child");
	int status = -1;
	require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		WEXITSTATUS(status) == EXIT_SUCCESS,
		"native registry OOM child did not fail closed and retry");
}

} // namespace

int main(int argc, char **argv)
{
	try {
		static_assert(page_size == 16384);
		uml::testing::NativeTestWindowScope native_window;
		require(native_window.entered(),
			"could not configure native ownership test window");
		if (argc == 2 &&
		    (std::string_view{argv[1]} ==
			"--native-domain-registry-oom-child" ||
		     std::string_view{argv[1]} ==
			"--native-owner-registry-oom-child"))
			return run_registry_oom_child(argv[1]);

		require(argc >= 1 && argv[0] != nullptr,
			"native ownership test has no executable path");
		require_registry_oom_child(
			argv[0], "--native-domain-registry-oom-child");
		require_registry_oom_child(
			argv[0], "--native-owner-registry-oom-child");

		require(counts_are_zero(),
			"native ownership test started with retained state");
		NativeWindowArena arena{28};
		test_nonoverlap_concurrency_and_leaked_cleanup(arena);
		test_same_address_collision_and_retry(arena);
		test_retire_acquire_handoff(arena);
		test_same_owner_replacement(arena);
		test_mixed_foreign_range_is_atomic(arena);
		test_unowned_host_mapping_is_replaced(arena);
		test_owned_bridge_lifecycle(arena);
		test_direct_software_publication_is_identity(arena);
		test_direct_retry_validates_ownership_mapping_and_permissions(arena);
		test_two_page_rollback(arena);
		run_failure_retry(arena, 17,
			uml::memory::testing::AllocationSite::none, false);
		run_failure_retry(arena, 18,
			uml::memory::testing::AllocationSite::native_direct_record, true);
		run_failure_retry(arena, 19,
			uml::memory::testing::AllocationSite::native_owner_record, true);
		run_failure_retry(arena, 20,
			uml::memory::testing::AllocationSite::native_after_mmap, true);
		require(counts_are_zero(),
			"native ownership test retained final state");
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "native 16K ownership test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
}
