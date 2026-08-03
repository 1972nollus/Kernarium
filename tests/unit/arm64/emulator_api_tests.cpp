// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/emulator.h"
#include "runtime/context.hpp"
#include "uml_darwin_cpu.h"
#include "uml_darwin_network.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <sys/sysctl.h>
#include <thread>

extern "C" int darwin_uml_net_open(unsigned char mac[6], unsigned int *mtu);
extern "C" int darwin_uml_net_irq_fd(void);
extern "C" int darwin_uml_net_read(void *buffer, unsigned long capacity);
extern "C" int darwin_uml_net_write(const void *buffer, unsigned long length);

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

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error{message};
}

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

bool current_stop_requested()
{
	const auto *context = ::uml::runtime::current_context();
	return context != nullptr && context->stop_requested();
}

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

int concurrent_kernel(darwin_uml_execution_engine expected_engine,
	unsigned int expected_mtu, bool expected_stop)
{
	unsigned char mac[6]{};
	unsigned int mtu{};
	const bool engine_isolated =
		darwin_uml_get_execution_engine() == expected_engine;
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
			darwin_uml_get_execution_engine() ==
				DARWIN_UML_EXECUTION_AUTO &&
			darwin_uml_set_execution_engine(expected_engine) == -1 &&
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
		DARWIN_UML_EXECUTION_GADGETS, 1401, true);
	concurrent_a_valid = status == 0;
	return status;
}

int concurrent_kernel_b(int, char *const[], char *const[])
{
	const auto status = concurrent_kernel(
		DARWIN_UML_EXECUTION_INTERPRETER, 1402, false);
	concurrent_b_valid = status == 0;
	return status;
}

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

#if UML_EMULATOR_TEST_HAS_SLIRP
void test_default_network_requests_dhcp()
{
	const uml_emulator_config config{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = "/tmp/rootfs.img",
		.kernel_image = nullptr,
		.kernel_image_size = 0,
		.native_kernel_entry = native_dhcp_kernel,
		.network = nullptr,
	};
	uml_emulator *created = nullptr;
	require(uml_emulator_create(&config, &created) == UML_EMULATOR_OK,
		"default-network emulator creation failed");
	const std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)> emulator{
		created, uml_emulator_destroy};
	const char *arguments[]{"uml-test"};
	require(uml_emulator_run(emulator.get(), 1, arguments, nullptr) == 0,
		"default-network emulator run failed");
	require(dhcp_argument_present,
		"default user network did not request kernel DHCP");
}
#endif

} // namespace

int main()
{
	try {
		test_native_entry();
		test_handle_lifetime_isolation();
#if UML_EMULATOR_TEST_HAS_SLIRP
		test_default_network_requests_dhcp();
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
