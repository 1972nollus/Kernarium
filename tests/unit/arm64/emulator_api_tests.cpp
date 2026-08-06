// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/emulator.h"
#include "allocation_failure.hpp"
#include "device/network/device.hpp"
#include "platform/run_routing.hpp"
#include "runtime/context.hpp"
#include "uml_darwin_cpu.h"
#include "uml_darwin_network.h"
#include "uml_darwin_memory.h"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <sys/sysctl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

extern "C" int darwin_uml_net_open(unsigned char mac[6], unsigned int *mtu);
extern "C" int darwin_uml_net_irq_fd(void);
extern "C" int darwin_uml_net_read(void *buffer, unsigned long capacity);
extern "C" int darwin_uml_net_write(const void *buffer, unsigned long length);
#if UML_EMULATOR_TEST_HAS_SLIRP
extern "C" int uml_slirp_net_open(
	void *, unsigned char[6], unsigned int *);
extern "C" int uml_slirp_net_irq_fd(void *);
extern "C" int uml_slirp_net_read(void *, void *, std::size_t);
extern "C" int uml_slirp_net_write(void *, const void *, std::size_t);
extern "C" void uml_slirp_net_stop(void *);
extern "C" void *uml_slirp_net_create(void) noexcept;
extern "C" void uml_slirp_net_destroy(void *) noexcept;
#endif

namespace {

bool native_arguments_valid;
bool execution_option_was_stripped;
bool native_inputs_were_mutable;
#if UML_EMULATOR_TEST_HAS_SLIRP
bool dhcp_argument_present;
#endif

struct NetworkState final {
	int writes{};
	unsigned int mtu{1500};
};

#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
struct ConcurrentCoordinator final {
	std::mutex mutex;
	std::condition_variable changed;
	std::size_t entered{};
	std::size_t unbound_ready{};
	bool sample_stop{};
};

ConcurrentCoordinator concurrent;
bool concurrent_a_valid;
bool concurrent_b_valid;

struct MmuCoordinator final {
	std::mutex mutex;
	std::condition_variable changed;
	std::size_t mapped{};
} mmu_concurrent;

struct MmuKernelState final {
	int fd{-1};
	std::uint8_t expected{};
	bool valid{};
};

MmuKernelState mmu_a;
MmuKernelState mmu_b;
#endif

#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
struct NativeShadowKernelState final {
	std::uintptr_t address{};
	int fd{-1};
	std::uint8_t expected{};
	std::uint8_t replacement{};
	int status{};
	bool valid{};
};

NativeShadowKernelState native_shadow_a;
NativeShadowKernelState native_shadow_b;
#endif

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error{message};
}

void test_c_api_rejects_inconsistent_creation_inputs()
{
	auto *stale = reinterpret_cast<uml_emulator *>(std::uintptr_t{1});
	require(uml_emulator_create(nullptr, &stale) ==
			-UML_EMULATOR_INVALID_ARGUMENT && stale == nullptr,
		"failed creation did not clear the caller's output handle");

	const uml_emulator_config inconsistent_image{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = "/tmp/rootfs.img",
		.kernel_image = nullptr,
		.kernel_image_size = 1,
	};
	stale = reinterpret_cast<uml_emulator *>(std::uintptr_t{1});
	require(uml_emulator_create(&inconsistent_image, &stale) ==
			-UML_EMULATOR_INVALID_ARGUMENT && stale == nullptr,
		"null kernel image with a positive size was accepted");
}

void test_memory_bridge_rejects_split_address_space_ranges()
{
	constexpr std::uintptr_t address_limit = 0x6000'0000'0000ULL;
	constexpr std::uintptr_t low_page =
		address_limit - DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr unsigned long split_length = 2 * DARWIN_UML_GUEST_PAGE_SIZE;
	const auto *address = reinterpret_cast<void *>(low_page);
	const auto rejected = [](int result) {
		return result == -1 && errno == EINVAL;
	};

	errno = 0;
	require(rejected(darwin_uml_vm_map(const_cast<void *>(address), -1, 0,
		split_length, PROT_READ, 0, 0)),
		"split static/native map was routed to one address-space implementation");
	errno = 0;
	require(rejected(darwin_uml_vm_map_generation(17,
		const_cast<void *>(address), -1, 0, split_length,
		PROT_READ, 0, 0)),
		"split generation map was routed to one address-space implementation");
	errno = 0;
	require(rejected(darwin_uml_vm_protect(
		const_cast<void *>(address), split_length, PROT_READ)),
		"split protect was routed to one address-space implementation");
	errno = 0;
	require(rejected(darwin_uml_vm_unmap(
		const_cast<void *>(address), split_length)),
		"split unmap was routed to one address-space implementation");
	errno = 0;
	require(rejected(darwin_uml_vm_unmap_generation(
		17, const_cast<void *>(address), split_length)),
		"split generation unmap was routed to one address-space implementation");
	errno = 0;
	require(rejected(darwin_uml_vm_drop(
		const_cast<void *>(address), split_length)),
		"split drop was silently accepted");
	errno = 0;
	require(rejected(darwin_uml_vm_sync_from_backing(address_limit - 1)),
		"split backing refresh was silently accepted");
	errno = 0;
	require(rejected(darwin_uml_vm_sync_from_buffer(
		low_page, nullptr, split_length)),
		"split copy range was not rejected before address-space access");
}

[[nodiscard]] uml::runtime::ExecutionEngine current_execution_engine() noexcept
{
	const auto *context = uml::runtime::current_context();
	return context == nullptr ? uml::runtime::ExecutionEngine::automatic :
		context->execution_engine();
}

#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
[[nodiscard]] int select_current_execution_engine(
	uml::runtime::ExecutionEngine engine) noexcept
{
	auto *context = uml::runtime::current_context();
	return context == nullptr ? -1 : context->set_execution_engine(engine);
}
#endif

class TemporaryBacking final {
public:
	explicit TemporaryBacking(std::uint8_t value)
	{
		char path[] = "/tmp/uml-emulator-mmu-domain-XXXXXX";
		fd_ = mkstemp(path);
		require(fd_ >= 0, "could not create MMU backing");
		require(unlink(path) == 0, "could not unlink MMU backing");
		require(ftruncate(fd_, DARWIN_UML_GUEST_PAGE_SIZE) == 0,
			"could not size MMU backing");
		require(pwrite(fd_, &value, sizeof(value), 0) ==
			static_cast<ssize_t>(sizeof(value)),
			"could not initialize MMU backing");
	}

	~TemporaryBacking()
	{
		if (fd_ >= 0)
			close(fd_);
	}

	TemporaryBacking(const TemporaryBacking &) = delete;
	TemporaryBacking &operator=(const TemporaryBacking &) = delete;

	[[nodiscard]] int fd() const noexcept { return fd_; }

private:
	int fd_{-1};
};

#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
class ReservedNativeSlot final {
public:
	ReservedNativeSlot()
		: size_{static_cast<std::size_t>(getpagesize())}
	{
		void *reservation = mmap(
			reinterpret_cast<void *>(0x7f00'0000'0000ULL), size_,
			PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
		require(reservation != MAP_FAILED,
			"could not reserve native-shadow host slot");
		address_ = reinterpret_cast<std::uintptr_t>(reservation);
		require(address_ >= 0x6000'0000'0000ULL,
			"native-shadow slot remained in guest address space");
	}

	~ReservedNativeSlot()
	{
		if (!released_)
			(void)munmap(reinterpret_cast<void *>(address_), size_);
	}

	ReservedNativeSlot(const ReservedNativeSlot &) = delete;
	ReservedNativeSlot &operator=(const ReservedNativeSlot &) = delete;

	void release()
	{
		require(!released_ &&
			munmap(reinterpret_cast<void *>(address_), size_) == 0,
			"could not release native-shadow slot reservation");
		released_ = true;
	}

	[[nodiscard]] std::uintptr_t page() const noexcept
	{
		return address_ + DARWIN_UML_GUEST_PAGE_SIZE;
	}

private:
	std::uintptr_t address_{};
	std::size_t size_{};
	bool released_{};
};

int native_shadow_kernel(NativeShadowKernelState &state)
{
	state.valid = darwin_uml_vm_map(
		reinterpret_cast<void *>(state.address), state.fd, 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0;
	if (state.valid)
		state.valid = *reinterpret_cast<volatile std::uint8_t *>(
			state.address) == state.expected;
	if (state.valid)
		*reinterpret_cast<volatile std::uint8_t *>(state.address) =
			state.replacement;
	return state.status;
}

int native_shadow_kernel_a(int, char *const[], char *const[])
{
	return native_shadow_kernel(native_shadow_a);
}

int native_shadow_kernel_b(int, char *const[], char *const[])
{
	return native_shadow_kernel(native_shadow_b);
}

[[nodiscard]] bool native_shadow_absent(std::uintptr_t address)
{
	errno = 0;
	return darwin_uml_vm_protect(reinterpret_cast<void *>(address),
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ) == -1;
}
#endif

bool host_feature(const char *name)
{
	int enabled = 0;
	std::size_t size = sizeof(enabled);
	return sysctlbyname(name, &enabled, &size, nullptr, 0) == 0 &&
		enabled != 0;
}

unsigned long expected_guest_hwcap()
{
	unsigned long result =
		(1UL << 0) | (1UL << 1);
	/* FP and ASIMD are the mandatory guest profile. */
	const struct {
		const char *sysctl;
		unsigned bit;
	} optional[]{
		{"hw.optional.arm.FEAT_AES", 3},
		{"hw.optional.arm.FEAT_PMULL", 4},
		{"hw.optional.arm.FEAT_SHA1", 5},
		{"hw.optional.arm.FEAT_SHA256", 6},
		{"hw.optional.arm.FEAT_CRC32", 7},
		{"hw.optional.arm.FEAT_LSE", 8},
		{"hw.optional.arm.FEAT_SHA3", 17},
		{"hw.optional.arm.FEAT_SHA512", 21},
	};
	for (const auto &feature : optional)
		if (host_feature(feature.sysctl))
			result |= 1UL << feature.bit;
	return result;
}

int native_kernel(int argc, char *const argv[], char *const envp[])
{
	const auto arguments = std::span{argv, static_cast<std::size_t>(argc)};
	const bool rootfs = std::ranges::any_of(arguments, [](const char *argument) {
		return std::strcmp(argument, "ubd0r=/tmp/rootfs.img") == 0;
	});
	const bool root = std::ranges::any_of(arguments, [](const char *argument) {
		return std::strcmp(argument, "root=/dev/ubda1") == 0;
	});
	const bool environment = envp != nullptr && envp[0] != nullptr &&
		std::strcmp(envp[0], "UML_TEST=1") == 0;
	execution_option_was_stripped = std::ranges::none_of(
		arguments, [](const char *argument) {
			return std::string_view{argument}.starts_with(
				"--execution-engine");
		});
	native_arguments_valid = rootfs && root && environment;
	unsigned char mac[6]{};
	unsigned int mtu{};
	const unsigned char frame[4]{1, 2, 3, 4};
	native_arguments_valid = native_arguments_valid &&
		darwin_uml_net_open(mac, &mtu) == 0 && mtu == 1500 &&
		darwin_uml_net_irq_fd() == 42 &&
		darwin_uml_net_read(nullptr, 0) == 0 &&
		darwin_uml_net_write(frame, sizeof(frame)) == 0;
	argv[0][0] = 'U';
	envp[0][0] = 'u';
	native_inputs_were_mutable = argv[0][0] == 'U' && envp[0][0] == 'u';
	return 73;
}

#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
bool current_stop_requested()
{
	const auto *context = ::uml::runtime::current_context();
	return context != nullptr && context->stop_requested();
}
#endif

int network_open(void *, uint8_t mac[6], unsigned int *mtu)
{
	std::fill_n(mac, 6, uint8_t{0x52});
	*mtu = 1500;
	return 0;
}

int instance_network_open(void *context, uint8_t mac[6], unsigned int *mtu)
{
	const auto &state = *static_cast<NetworkState *>(context);
	std::fill_n(mac, 6, uint8_t{0x52});
	*mtu = state.mtu;
	return 0;
}

int network_irq_fd(void *) { return 42; }
int network_read(void *, void *, size_t) { return 0; }

int network_write(void *context, const void *, size_t)
{
	static_cast<NetworkState *>(context)->writes++;
	return 0;
}

#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
int concurrent_kernel(uml::runtime::ExecutionEngine expected_engine,
	unsigned int expected_mtu, bool expected_stop)
{
	unsigned char mac[6]{};
	unsigned int mtu{};
	const bool engine_isolated =
		current_execution_engine() ==
			expected_engine;
	const bool network_isolated =
		darwin_uml_net_open(mac, &mtu) == 0 && mtu == expected_mtu;
	{
		std::unique_lock lock{concurrent.mutex};
		++concurrent.entered;
		concurrent.changed.notify_all();
		if (!concurrent.changed.wait_for(lock, std::chrono::seconds{5}, [] {
			return concurrent.sample_stop;
		}))
			return 1;
	}
	bool unbound_failed_closed = false;
	std::thread unbound{[&] {
		{
			std::unique_lock lock{concurrent.mutex};
			++concurrent.unbound_ready;
			concurrent.changed.notify_all();
			if (!concurrent.changed.wait_for(
				lock, std::chrono::seconds{5}, [] {
					return concurrent.unbound_ready == 2;
				}))
				return;
		}
		unsigned char unbound_mac[6]{};
		unsigned int unbound_mtu{};
		unbound_failed_closed =
			current_execution_engine() ==
				uml::runtime::ExecutionEngine::automatic &&
			select_current_execution_engine(
				expected_engine) == -1 &&
			darwin_uml_net_open(unbound_mac, &unbound_mtu) == -ENETDOWN &&
			!current_stop_requested();
	}};
	unbound.join();
	return engine_isolated && network_isolated &&
		current_stop_requested() == expected_stop &&
		unbound_failed_closed ? 0 : 1;
}

int concurrent_kernel_a(int, char *const[], char *const[])
{
	const auto status = concurrent_kernel(
		uml::runtime::ExecutionEngine::gadgets, 1401, true);
	concurrent_a_valid = status == 0;
	return status;
}

int concurrent_kernel_b(int, char *const[], char *const[])
{
	const auto status = concurrent_kernel(
		uml::runtime::ExecutionEngine::interpreter, 1402, false);
	concurrent_b_valid = status == 0;
	return status;
}

int isolated_mmu_kernel(MmuKernelState &state)
{
	constexpr unsigned long generation = 0x4d4d;
	constexpr unsigned long address = 0x80'0000;
	const bool mapped = darwin_uml_static_vm_map_generation(
		generation, reinterpret_cast<void *>(address), state.fd, 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0;
	{
		std::unique_lock lock{mmu_concurrent.mutex};
		++mmu_concurrent.mapped;
		mmu_concurrent.changed.notify_all();
		if (!mmu_concurrent.changed.wait_for(
			lock, std::chrono::seconds{5}, [] {
				return mmu_concurrent.mapped == 2;
			}))
			return 1;
	}
	void *translated = nullptr;
	state.valid = mapped &&
		darwin_uml_static_vm_translate(
			generation, address, DARWIN_UML_STATIC_READ, &translated) ==
			DARWIN_UML_STATIC_TRANSLATED &&
		translated != nullptr &&
		*static_cast<const std::uint8_t *>(translated) == state.expected;
	return state.valid ? 0 : 1;
}

int isolated_mmu_kernel_a(int, char *const[], char *const[])
{
	return isolated_mmu_kernel(mmu_a);
}

int isolated_mmu_kernel_b(int, char *const[], char *const[])
{
	return isolated_mmu_kernel(mmu_b);
}
#endif

#if UML_EMULATOR_TEST_HAS_SLIRP
int native_dhcp_kernel(int argc, char *const argv[], char *const[])
{
	const auto arguments = std::span{argv, static_cast<std::size_t>(argc)};
	dhcp_argument_present = std::ranges::any_of(
		arguments, [](const char *argument) {
			return std::strcmp(argument, "ip=dhcp") == 0;
		});
	return 0;
}
#endif

void test_native_entry()
{
	const auto expected_hwcap = expected_guest_hwcap();
	require(darwin_uml_guest_hwcap() == expected_hwcap,
		"guest HWCAP does not match the host-safe virtual CPU");
	require(darwin_uml_guest_hwcap2() == 0,
		"guest exposed an unimplemented HWCAP2 extension");

	NetworkState network_state;
	uml_emulator_network_callbacks network{
		.struct_size = sizeof(uml_emulator_network_callbacks),
		.context = &network_state,
		.open = network_open,
		.irq_fd = network_irq_fd,
		.read = network_read,
		.write = network_write,
	};
	char rootfs_path[]{"/tmp/rootfs.img"};
	uml_emulator_config config{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = rootfs_path,
		.kernel_image = nullptr,
		.kernel_image_size = 0,
		.native_kernel_entry = native_kernel,
		.network = &network,
	};
	uml_emulator *created = nullptr;
	require(uml_emulator_create(&config, &created) == UML_EMULATOR_OK,
		"emulator creation failed");
	const std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)> emulator{
		created, uml_emulator_destroy};
	rootfs_path[0] = 'X';
	network.open = nullptr;
	config.rootfs_path = nullptr;
	config.native_kernel_entry = nullptr;
	config.network = nullptr;
	uml_emulator_capabilities capabilities{
		.struct_size = sizeof(uml_emulator_capabilities),
	};
	require(uml_emulator_get_capabilities(emulator.get(), &capabilities) ==
		UML_EMULATOR_OK, "capability query failed");
	require(capabilities.can_run == 1,
		"native entry was not reported as runnable");
	require(capabilities.kernel_is_native_macho == 1,
		"native entry was not reported as Mach-O");
	require(capabilities.sandbox_safe == 1,
		"native entry was not reported as sandbox safe");
	require(capabilities.userspace_execution == UML_EMULATOR_NATIVE_JIT,
		"macOS did not default to the native JIT");
	char argument_zero[]{"uml-test"};
	char argument_one[]{"--execution-engine=gadgets"};
	char environment_zero[]{"UML_TEST=1"};
	const char *arguments[]{argument_zero, argument_one};
	const char *environment[]{environment_zero, nullptr};
	require(uml_emulator_run(emulator.get(), 2, arguments, environment) == 73,
		"native entry result was not returned");
	require(native_inputs_were_mutable &&
		std::strcmp(argument_zero, "uml-test") == 0 &&
		std::strcmp(argument_one, "--execution-engine=gadgets") == 0 &&
		std::strcmp(environment_zero, "UML_TEST=1") == 0,
		"run did not isolate caller-owned argument/environment buffers");
	require(uml_emulator_get_capabilities(emulator.get(), &capabilities) ==
		UML_EMULATOR_OK &&
		capabilities.userspace_execution == UML_EMULATOR_STATIC_GADGETS,
		"handle-local gadget selection was not retained");
	require(darwin_uml_guest_hwcap() == expected_hwcap,
		"gadget selection changed the virtual CPU contract");
	require(execution_option_was_stripped,
		"host execution option leaked into kernel arguments");
	require(native_arguments_valid,
		"native entry arguments or network callbacks were invalid");
	require(network_state.writes == 1,
		"network callback was not invoked exactly once");

	const char *invalid_arguments[]{
		"uml-test", "--execution-engine=invalid"};
	require(uml_emulator_run(
			emulator.get(), 2, invalid_arguments, environment) ==
		-UML_EMULATOR_INVALID_ARGUMENT,
		"invalid execution engine was accepted");
	const char *invalid_fallback[]{
		"uml-test", "--jit-gadget-fallback=off"};
	require(uml_emulator_run(
			emulator.get(), 2, invalid_fallback, environment) ==
		-UML_EMULATOR_INVALID_ARGUMENT,
		"removed JIT gadget fallback option was accepted");
}

#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
void test_handle_lifetime_isolation()
{
	NetworkState network_a{.mtu = 1401};
	NetworkState network_b{.mtu = 1402};
	const uml_emulator_network_callbacks callbacks_a{
		.struct_size = sizeof(uml_emulator_network_callbacks),
		.context = &network_a,
		.open = instance_network_open,
		.irq_fd = network_irq_fd,
		.read = network_read,
		.write = network_write,
	};
	const uml_emulator_network_callbacks callbacks_b{
		.struct_size = sizeof(uml_emulator_network_callbacks),
		.context = &network_b,
		.open = instance_network_open,
		.irq_fd = network_irq_fd,
		.read = network_read,
		.write = network_write,
	};
	const uml_emulator_config config_a{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = "/tmp/handle-a.img",
		.native_kernel_entry = concurrent_kernel_a,
		.network = &callbacks_a,
	};
	const uml_emulator_config config_b{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = "/tmp/handle-b.img",
		.native_kernel_entry = concurrent_kernel_b,
		.network = &callbacks_b,
	};
	uml_emulator *raw_a = nullptr;
	uml_emulator *raw_b = nullptr;
	require(uml_emulator_create(&config_a, &raw_a) == UML_EMULATOR_OK &&
		uml_emulator_create(&config_b, &raw_b) == UML_EMULATOR_OK,
		"two-handle emulator creation failed");
	const std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)> handle_a{
		raw_a, uml_emulator_destroy};
	const std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)> handle_b{
		raw_b, uml_emulator_destroy};

	/* An idle stop request is not inherited by a later run. */
	uml_emulator_request_stop(handle_a.get());
	int status_a = -1;
	int status_b = -1;
	const char *arguments_a[]{"uml-test", "--execution-engine=gadgets"};
	const char *arguments_b[]{"uml-test", "--execution-engine=interpreter"};
	std::thread thread_a{[&] {
		status_a = uml_emulator_run(handle_a.get(), 2, arguments_a, nullptr);
	}};
	std::thread thread_b{[&] {
		status_b = uml_emulator_run(handle_b.get(), 2, arguments_b, nullptr);
	}};
	bool both_entered = false;
	{
		std::unique_lock lock{concurrent.mutex};
		both_entered = concurrent.changed.wait_for(
			lock, std::chrono::seconds{5}, [] {
				return concurrent.entered == 2;
			});
	}
	uml_emulator_capabilities live_capabilities_a{
		.struct_size = sizeof(uml_emulator_capabilities),
	};
	uml_emulator_capabilities live_capabilities_b{
		.struct_size = sizeof(uml_emulator_capabilities),
	};
	const bool live_capabilities_valid = both_entered &&
		uml_emulator_get_capabilities(
			handle_a.get(), &live_capabilities_a) == UML_EMULATOR_OK &&
		uml_emulator_get_capabilities(
			handle_b.get(), &live_capabilities_b) == UML_EMULATOR_OK &&
		live_capabilities_a.userspace_execution ==
			UML_EMULATOR_STATIC_GADGETS &&
		live_capabilities_b.userspace_execution ==
			UML_EMULATOR_INTERPRETER;
	if (both_entered)
		uml_emulator_request_stop(handle_a.get());
	{
		std::lock_guard lock{concurrent.mutex};
		concurrent.sample_stop = true;
	}
	concurrent.changed.notify_all();
	thread_a.join();
	thread_b.join();

	if (!both_entered || status_a != 0 || status_b != 0 ||
	    !concurrent_a_valid || !concurrent_b_valid)
		std::fprintf(stderr,
			"two-handle diagnostics entered=%d status-a=%d status-b=%d "
			"valid-a=%d valid-b=%d\n",
			both_entered, status_a, status_b, concurrent_a_valid,
			concurrent_b_valid);
	require(both_entered && status_a == 0 && status_b == 0 &&
		concurrent_a_valid && concurrent_b_valid,
		"concurrent handles shared engine, stop, or network state");
	require(live_capabilities_valid,
		"capability query did not remain stable during live runs");
	uml_emulator_capabilities capabilities_a{
		.struct_size = sizeof(uml_emulator_capabilities),
	};
	uml_emulator_capabilities capabilities_b{
		.struct_size = sizeof(uml_emulator_capabilities),
	};
	require(uml_emulator_get_capabilities(handle_a.get(), &capabilities_a) ==
		UML_EMULATOR_OK &&
		uml_emulator_get_capabilities(handle_b.get(), &capabilities_b) ==
		UML_EMULATOR_OK &&
		capabilities_a.userspace_execution == UML_EMULATOR_STATIC_GADGETS &&
		capabilities_b.userspace_execution == UML_EMULATOR_INTERPRETER,
		"two handles did not retain independent engine selection");
}

void test_concurrent_handles_have_distinct_software_mmu_domains()
{
	TemporaryBacking backing_a{0x2a};
	TemporaryBacking backing_b{0xb7};
	mmu_a = {.fd = backing_a.fd(), .expected = 0x2a};
	mmu_b = {.fd = backing_b.fd(), .expected = 0xb7};
	{
		std::lock_guard lock{mmu_concurrent.mutex};
		mmu_concurrent.mapped = 0;
	}
	const uml_emulator_config config_a{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = "/tmp/mmu-domain-a.img",
		.native_kernel_entry = isolated_mmu_kernel_a,
	};
	const uml_emulator_config config_b{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = "/tmp/mmu-domain-b.img",
		.native_kernel_entry = isolated_mmu_kernel_b,
	};
	uml_emulator *raw_a = nullptr;
	uml_emulator *raw_b = nullptr;
	require(uml_emulator_create(&config_a, &raw_a) == UML_EMULATOR_OK &&
		uml_emulator_create(&config_b, &raw_b) == UML_EMULATOR_OK,
		"MMU-domain emulator creation failed");
	const std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)> handle_a{
		raw_a, uml_emulator_destroy};
	const std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)> handle_b{
		raw_b, uml_emulator_destroy};
	const char *arguments[]{"uml-mmu-domain"};
	int status_a = -1;
	int status_b = -1;
	std::thread thread_a{[&] {
		status_a = uml_emulator_run(handle_a.get(), 1, arguments, nullptr);
	}};
	std::thread thread_b{[&] {
		status_b = uml_emulator_run(handle_b.get(), 1, arguments, nullptr);
	}};
	thread_a.join();
	thread_b.join();
	require(status_a == 0 && status_b == 0 && mmu_a.valid && mmu_b.valid,
		"concurrent handles shared software-MMU generation state");
}
#endif

#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
void test_native_shadow_domains_retire_on_every_handle_exit()
{
	TemporaryBacking backing_a{0x31};
	TemporaryBacking backing_b{0xa7};
	ReservedNativeSlot slot;
	slot.release();
	native_shadow_a = {
		.address = slot.page(),
		.fd = backing_a.fd(),
		.expected = 0x31,
		.replacement = 0x42,
		.status = 0,
	};
	native_shadow_b = {
		.address = slot.page(),
		.fd = backing_b.fd(),
		.expected = 0xa7,
		.replacement = 0xb8,
		.status = 37,
	};
	const uml_emulator_config config_a{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = "/tmp/native-shadow-a.img",
		.native_kernel_entry = native_shadow_kernel_a,
	};
	const uml_emulator_config config_b{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = "/tmp/native-shadow-b.img",
		.native_kernel_entry = native_shadow_kernel_b,
	};
	uml_emulator *raw_a = nullptr;
	uml_emulator *raw_b = nullptr;
	require(uml_emulator_create(&config_a, &raw_a) == UML_EMULATOR_OK &&
		uml_emulator_create(&config_b, &raw_b) == UML_EMULATOR_OK,
		"native-shadow handle creation failed");
	const std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)> handle_a{
		raw_a, uml_emulator_destroy};
	const std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)> handle_b{
		raw_b, uml_emulator_destroy};
	const char *arguments[]{"native-shadow-domain"};
	require(uml_emulator_run(handle_a.get(), 1, arguments, nullptr) == 0 &&
		native_shadow_a.valid,
		"normal native-shadow run failed");
	require(native_shadow_absent(slot.page()),
		"normal run retained native-shadow mapping or protection state");
	std::uint8_t backing_value{};
	require(pread(backing_a.fd(), &backing_value, sizeof(backing_value), 0) ==
			static_cast<ssize_t>(sizeof(backing_value)) &&
		backing_value == 0x42,
		"normal native-shadow retirement did not flush backing state");

	require(uml_emulator_run(handle_b.get(), 1, arguments, nullptr) == 37 &&
		native_shadow_b.valid,
		"abnormal-return native-shadow run failed");
	require(native_shadow_absent(slot.page()),
		"abnormal return retained native-shadow state");
	require(pread(backing_b.fd(), &backing_value, sizeof(backing_value), 0) ==
			static_cast<ssize_t>(sizeof(backing_value)) &&
		backing_value == 0xb8,
		"abnormal native-shadow retirement did not flush backing state");

	native_shadow_b.expected = 0xb8;
	native_shadow_b.replacement = 0xc9;
	native_shadow_b.status = 0;
	native_shadow_b.valid = false;
	require(uml_emulator_run(handle_b.get(), 1, arguments, nullptr) == 0 &&
		native_shadow_b.valid && native_shadow_absent(slot.page()),
		"handle could not retry with a fresh native-shadow domain");
}

#endif

void test_active_registries_are_allocation_free_and_transactional()
{
	NetworkState state_a{.mtu = 1701};
	NetworkState state_b{.mtu = 1702};
	uml::network::ProviderContext provider_a{{
		.context = &state_a,
		.open = instance_network_open,
		.irq_fd = network_irq_fd,
		.read = network_read,
		.write = network_write,
	}};
	uml::network::ProviderContext provider_b{{
		.context = &state_b,
		.open = instance_network_open,
		.irq_fd = network_irq_fd,
		.read = network_read,
		.write = network_write,
	}};
	uml::runtime::Context context_a;
	uml::runtime::Context context_b;
	require(context_a.set_execution_engine(
			uml::runtime::ExecutionEngine::interpreter) == 0,
		"could not select registry test engine");
	uml::runtime::RunScope run_a{context_a};
	require(run_a.entered(), "registry test run was not admitted");
	uml::routing::ActiveRouteScope active_a{&context_a, &provider_a};

	const auto a_is_still_unbound_fallback = [&] {
		bool valid = false;
		std::thread sample{[&] {
			unsigned char mac[6]{};
			unsigned int mtu{};
			valid = current_execution_engine() ==
					uml::runtime::ExecutionEngine::interpreter &&
				darwin_uml_net_open(mac, &mtu) == 0 && mtu == 1701;
		}};
		sample.join();
		return valid;
	};
	require(a_is_still_unbound_fallback(),
		"handle A was not the initial unbound fallback");

	/* The route reservation used by Emulator::run() is allocation-free. A
	 * pending pair stays invisible and rolls back without mutating the
	 * established one-run fallback. */
	uml::testing::fail_next_allocation();
	{
		uml::routing::ActiveRouteScope pending_b{
			&context_b, &provider_b,
			uml::routing::ActiveRouteScope::DeferredPublication{}};
		require(uml::testing::cancel_pending_allocation_failure(),
			"route reservation unexpectedly allocated");
	}
	require(a_is_still_unbound_fallback(),
		"pending route did not roll back exactly");

	require(context_b.set_execution_engine(
			uml::runtime::ExecutionEngine::gadgets) == 0,
		"could not select retry engine for context B");
	{
		uml::routing::ActiveRouteScope retry_b{
			&context_b, &provider_b,
			uml::routing::ActiveRouteScope::DeferredPublication{}};
		retry_b.commit();
		unsigned char mac[6]{};
		unsigned int mtu{};
		require(current_execution_engine() ==
				uml::runtime::ExecutionEngine::gadgets &&
			darwin_uml_net_open(mac, &mtu) == 0 && mtu == 1702,
			"context/provider B could not retry after rollback");

		bool unbound_failed_closed = false;
		std::thread sample{[&] {
			unsigned char unbound_mac[6]{};
			unsigned int unbound_mtu{};
			unbound_failed_closed =
				current_execution_engine() ==
					uml::runtime::ExecutionEngine::automatic &&
				darwin_uml_net_open(unbound_mac, &unbound_mtu) ==
					-ENETDOWN;
		}};
		sample.join();
		require(unbound_failed_closed,
			"committed overlap did not fail closed");
	}
	require(!a_is_still_unbound_fallback(),
		"committed overlap incorrectly restored A's fallback");
}

#if UML_EMULATOR_TEST_HAS_SLIRP
void test_injected_user_network_requests_dhcp()
{
	void *network_context = uml_slirp_net_create();
	require(network_context != nullptr,
		"injected user-network creation failed");
	const std::unique_ptr<void, decltype(&uml_slirp_net_destroy)> network{
		network_context, uml_slirp_net_destroy};
	const uml_emulator_network_callbacks callbacks{
		.struct_size = sizeof(uml_emulator_network_callbacks),
		.context = network.get(),
		.open = uml_slirp_net_open,
		.irq_fd = uml_slirp_net_irq_fd,
		.read = uml_slirp_net_read,
		.write = uml_slirp_net_write,
	};
	const uml_emulator_config config{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = "/tmp/rootfs.img",
		.kernel_image = nullptr,
		.kernel_image_size = 0,
		.native_kernel_entry = native_dhcp_kernel,
		.network = &callbacks,
	};
	uml_emulator *created = nullptr;
	require(uml_emulator_create(&config, &created) == UML_EMULATOR_OK,
		"default-network emulator creation failed");
	const std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)> emulator{
		created, uml_emulator_destroy};
	const char *arguments[]{"uml-test"};
	require(uml_emulator_run(emulator.get(), 1, arguments, nullptr) == 0,
		"injected-network emulator run failed");
	uml_slirp_net_stop(network.get());
	require(dhcp_argument_present,
		"injected user network did not request kernel DHCP");
}
#endif

} // namespace

int main()
{
	try {
		test_c_api_rejects_inconsistent_creation_inputs();
		test_memory_bridge_rejects_split_address_space_ranges();
		test_native_entry();
	#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
		test_handle_lifetime_isolation();
		test_concurrent_handles_have_distinct_software_mmu_domains();
	#endif
	#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
		test_native_shadow_domains_retire_on_every_handle_exit();
	#endif
		test_active_registries_are_allocation_free_and_transactional();
#if UML_EMULATOR_TEST_HAS_SLIRP
		test_injected_user_network_requests_dhcp();
#endif
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "emulator API test failed: %s\n", error.what());
		return EXIT_FAILURE;
	} catch (...) {
		std::fprintf(stderr, "emulator API test failed: unknown exception\n");
		return EXIT_FAILURE;
	}
}
