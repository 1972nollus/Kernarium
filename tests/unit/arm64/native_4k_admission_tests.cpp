// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/emulator.h"
#include "domain_registry_testing.hpp"
#include "native_window_test_support.hpp"
#include "scratch_backing.hpp"
#include "testing.hpp"
#include "uml_darwin_abi.h"
#include "uml_darwin_memory.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

using uml::testing::ScratchBacking;

constexpr std::string_view backing_name = "uml-native-admission-XXXXXX";

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error{message};
}

class NativeSlot final {
public:
	NativeSlot() : size_{static_cast<std::size_t>(getpagesize())}
	{
		base_ = uml::testing::reserve_native_test_window(size_);
		require(base_ != 0,
			"could not reserve native admission slot");
		require(base_ >= uml::testing::native_test_window_start &&
			base_ <= uml::testing::native_test_window_end - size_,
			"native admission slot escaped the shared native window");
		require(uml::testing::release_native_test_window(base_, size_),
			"could not release native admission reservation");
	}

	~NativeSlot() = default;

	NativeSlot(const NativeSlot &) = delete;
	NativeSlot &operator=(const NativeSlot &) = delete;

	[[nodiscard]] std::uintptr_t page() const noexcept
	{
		return base_ + DARWIN_UML_GUEST_PAGE_SIZE;
	}

	void require_released() const
	{
		require(released(),
			"native admission slot remained mapped after run");
	}

	/* Retirement hands the slot back to the guest window's reservation, not
	 * to the process, so the slot carries no mapping and is not free. */
	[[nodiscard]] bool released() const noexcept
	{
		return uml::testing::released_into_native_window(base_, size_);
	}

private:
	std::uintptr_t base_{};
	std::size_t size_{};
};

struct KernelState final {
	std::uintptr_t address{};
	int fd{-1};
	std::uint8_t expected{};
	std::uint8_t replacement{};
	bool hold{};
	bool release{};
	bool entered{};
	bool valid{};
};

struct KernelCoordinator final {
	std::mutex mutex;
	std::condition_variable changed;
} kernel_coordinator;

KernelState state_a;
KernelState state_b;

int run_kernel(KernelState &state)
{
	bool valid = darwin_uml_vm_map(
		reinterpret_cast<void *>(state.address), state.fd, 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0;
	auto *byte = reinterpret_cast<volatile std::uint8_t *>(state.address);
	if (valid)
		valid = *byte == state.expected;
	if (valid)
		*byte = state.replacement;
	{
		std::lock_guard lock{kernel_coordinator.mutex};
		state.entered = true;
	}
	kernel_coordinator.changed.notify_all();
	if (state.hold) {
		std::unique_lock lock{kernel_coordinator.mutex};
		kernel_coordinator.changed.wait(lock, [&] { return state.release; });
	}
	if (valid)
		valid = *byte == state.replacement;
	state.valid = valid;
	return valid ? 0 : 91;
}

int kernel_a(int, char *const[], char *const[]) { return run_kernel(state_a); }
int kernel_b(int, char *const[], char *const[]) { return run_kernel(state_b); }

struct AdmissionState final {
	std::mutex mutex;
	std::condition_variable changed;
	const NativeSlot *retiring_slot{};
	int retiring_fd{-1};
	std::uint8_t retiring_value{};
	bool handle_admitted{};
	bool wait_boundary{};
	bool release_boundary{};
	bool retired_before_release{};
};

void after_handle_admission(void *opaque) noexcept
{
	auto &state = *static_cast<AdmissionState *>(opaque);
	std::lock_guard lock{state.mutex};
	state.handle_admitted = true;
	state.changed.notify_all();
}

void at_wait_boundary(void *opaque) noexcept
{
	auto &state = *static_cast<AdmissionState *>(opaque);
	std::lock_guard lock{state.mutex};
	state.wait_boundary = true;
	state.changed.notify_all();
}

void before_gate_release(void *opaque) noexcept
{
	auto &state = *static_cast<AdmissionState *>(opaque);
	std::uint8_t value{};
	const bool backing_flushed =
		state.retiring_fd >= 0 &&
		pread(state.retiring_fd, &value, sizeof(value), 0) ==
			static_cast<ssize_t>(sizeof(value)) &&
		value == state.retiring_value;
	const bool retired =
		uml::memory::testing::native_domain_count() == 0 &&
		state.retiring_slot != nullptr && state.retiring_slot->released() &&
		backing_flushed;
	{
		std::lock_guard lock{state.mutex};
		if (!state.release_boundary) {
			state.release_boundary = true;
			state.retired_before_release = retired;
		}
	}
	state.changed.notify_all();
}

class AdmissionHooks final {
public:
	explicit AdmissionHooks(AdmissionState &state) noexcept
	{
		uml::emulator::testing::set_address_space_admission_hooks(
			after_handle_admission, at_wait_boundary, before_gate_release,
			&state);
	}

	~AdmissionHooks()
	{
		uml::emulator::testing::set_address_space_admission_hooks(
			nullptr, nullptr, nullptr, nullptr);
	}

	AdmissionHooks(const AdmissionHooks &) = delete;
	AdmissionHooks &operator=(const AdmissionHooks &) = delete;
};

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
		"could not create native admission emulator");
	return {created, uml_emulator_destroy};
}

void reset_state(KernelState &state, std::uintptr_t address, int fd,
	std::uint8_t expected, std::uint8_t replacement, bool hold)
{
	std::lock_guard lock{kernel_coordinator.mutex};
	state = {
		.address = address,
		.fd = fd,
		.expected = expected,
		.replacement = replacement,
		.hold = hold,
	};
}

bool wait_entered(KernelState &state)
{
	std::unique_lock lock{kernel_coordinator.mutex};
	return kernel_coordinator.changed.wait_for(
		lock, 5s, [&] { return state.entered; });
}

void release(KernelState &state)
{
	{
		std::lock_guard lock{kernel_coordinator.mutex};
		state.release = true;
	}
	kernel_coordinator.changed.notify_all();
}

bool wait_for_admission(AdmissionState &state)
{
	std::unique_lock lock{state.mutex};
	return state.changed.wait_for(lock, 5s, [&] {
		return state.handle_admitted && state.wait_boundary;
	});
}

void test_same_address_is_serialized_and_isolated(const NativeSlot &slot)
{
	ScratchBacking backing_a{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing_a.put_byte(0, std::byte{0x19});
	ScratchBacking backing_b{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing_b.put_byte(0, std::byte{0xe3});
	reset_state(state_a, slot.page(), backing_a.descriptor(), 0x19, 0x2a, true);
	reset_state(state_b, slot.page(), backing_b.descriptor(), 0xe3, 0xf4, false);
	auto handle_a = make_emulator(kernel_a, "/tmp/native-gate-a.img");
	auto handle_b = make_emulator(kernel_b, "/tmp/native-gate-b.img");
	const char *arguments[]{"native-4k-same-address"};
	int status_a = -1;
	int status_b = -1;
	std::thread first{[&] {
		status_a = uml_emulator_run(handle_a.get(), 1, arguments, nullptr);
	}};
	require(wait_entered(state_a), "first same-address kernel did not enter");

	AdmissionState admission;
	admission.retiring_slot = &slot;
	admission.retiring_fd = backing_a.descriptor();
	admission.retiring_value = 0x2a;
	AdmissionHooks hooks{admission};
	std::thread second{[&] {
		status_b = uml_emulator_run(handle_b.get(), 1, arguments, nullptr);
	}};
	require(wait_for_admission(admission),
		"second same-address kernel did not reach the process gate");
	{
		std::lock_guard lock{kernel_coordinator.mutex};
		require(!state_b.entered,
			"same-address native callbacks overlapped in a 4 KiB build");
	}
	release(state_a);
	first.join();
	second.join();
	{
		std::lock_guard lock{admission.mutex};
		require(admission.release_boundary &&
			admission.retired_before_release,
			"native state was not retired before process-gate release");
	}
	require(status_a == 0 && status_b == 0 && state_a.valid && state_b.valid,
		"serialized same-address kernels observed crossed bytes or state");
	require(backing_a.get_byte(0) == std::byte{0x2a} &&
		backing_b.get_byte(0) == std::byte{0xf4},
		"serialized same-address kernels flushed the wrong backing");
	require(uml::memory::testing::native_domain_count() == 0,
		"same-address serialization retained a native MMU domain");
	slot.require_released();
}

void test_queued_stop_cancels_and_retry_is_fresh(const NativeSlot &slot)
{
	ScratchBacking backing_a{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing_a.put_byte(0, std::byte{0x35});
	ScratchBacking backing_b{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing_b.put_byte(0, std::byte{0xa6});
	reset_state(state_a, slot.page(), backing_a.descriptor(), 0x35, 0x46, true);
	reset_state(state_b, slot.page(), backing_b.descriptor(), 0xa6, 0xb7, false);
	auto handle_a = make_emulator(kernel_a, "/tmp/native-gate-stop-a.img");
	auto handle_b = make_emulator(kernel_b, "/tmp/native-gate-stop-b.img");
	const char *arguments[]{"native-4k-queued-stop"};
	int status_a = -1;
	int status_b = -1;
	bool second_finished{};
	std::thread first{[&] {
		status_a = uml_emulator_run(handle_a.get(), 1, arguments, nullptr);
	}};
	require(wait_entered(state_a), "queued-stop holder did not enter");

	AdmissionState admission;
	admission.retiring_slot = &slot;
	admission.retiring_fd = backing_a.descriptor();
	admission.retiring_value = 0x46;
	AdmissionHooks hooks{admission};
	{
		std::thread second{[&] {
			status_b = uml_emulator_run(
				handle_b.get(), 1, arguments, nullptr);
			{
				std::lock_guard lock{admission.mutex};
				second_finished = true;
			}
			admission.changed.notify_all();
		}};
		require(wait_for_admission(admission),
			"queued native run did not reach the process gate");
		require(uml_emulator_run(handle_b.get(), 1, arguments, nullptr) ==
				-UML_EMULATOR_RUNTIME_ERROR,
			"same-handle queued duplicate did not fail fast");
		uml_emulator_request_stop(handle_b.get());
		{
			std::unique_lock lock{admission.mutex};
			require(admission.changed.wait_for(lock, 1s, [&] {
				return second_finished;
			}), "queued stop did not cancel before gate release");
		}
		second.join();
	}
	{
		std::lock_guard lock{kernel_coordinator.mutex};
		require(!state_b.entered,
			"cancelled queued native callback entered the shared address space");
	}
	require(status_b == -UML_EMULATOR_RUNTIME_ERROR,
		"cancelled queued native run returned the wrong status");
	release(state_a);
	first.join();
	require(status_a == 0 && state_a.valid,
		"queued-stop holder did not retire cleanly");
	{
		std::lock_guard lock{admission.mutex};
		require(admission.release_boundary &&
			admission.retired_before_release,
			"queued holder released the gate before native retirement");
	}

	require(uml_emulator_run(handle_b.get(), 1, arguments, nullptr) == 0 &&
		state_b.entered && state_b.valid,
		"cancelled native handle could not retry with fresh state");
	require(backing_a.get_byte(0) == std::byte{0x46} &&
		backing_b.get_byte(0) == std::byte{0xb7},
		"queued stop or retry crossed native backing bytes");
	require(uml::memory::testing::native_domain_count() == 0,
		"queued stop or retry retained a native MMU domain");
	slot.require_released();
}

} // namespace

int main()
{
	try {
		static_assert(DARWIN_UML_GUEST_PAGE_SIZE == 4096);
		uml::testing::NativeTestWindowScope native_window;
		require(native_window.entered(),
			"could not configure native admission test window");
		require(uml::memory::testing::native_domain_count() == 0,
			"native admission test started with a retained domain");
		NativeSlot slot;
		test_same_address_is_serialized_and_isolated(slot);
		test_queued_stop_cancels_and_retry_is_fresh(slot);
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "native 4K admission test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
}
