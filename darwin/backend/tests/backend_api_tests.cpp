// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/backend.h"
#include "uml_darwin_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <sys/sysctl.h>

extern "C" int darwin_uml_net_open(unsigned char mac[6], unsigned int *mtu);
extern "C" int darwin_uml_net_irq_fd(void);
extern "C" int darwin_uml_net_read(void *buffer, unsigned long capacity);
extern "C" int darwin_uml_net_write(const void *buffer, unsigned long length);

namespace {

bool native_arguments_valid;
bool execution_option_was_stripped;
#if UML_BACKEND_TEST_HAS_SLIRP
bool dhcp_argument_present;
#endif

struct NetworkState final {
	int writes{};
};

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
		return std::strcmp(argument, "ubd0=/tmp/rootfs.ext2") == 0;
	});
	const bool root = std::ranges::any_of(arguments, [](const char *argument) {
		return std::strcmp(argument, "root=/dev/ubda") == 0;
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
	return 73;
}

int network_open(void *, uint8_t mac[6], unsigned int *mtu)
{
	std::fill_n(mac, 6, uint8_t{0x52});
	*mtu = 1500;
	return 0;
}

int network_irq_fd(void *) { return 42; }
int network_read(void *, void *, size_t) { return 0; }

int network_write(void *context, const void *, size_t)
{
	static_cast<NetworkState *>(context)->writes++;
	return 0;
}

#if UML_BACKEND_TEST_HAS_SLIRP
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
	const uml_backend_network_callbacks network{
		.struct_size = sizeof(uml_backend_network_callbacks),
		.context = &network_state,
		.open = network_open,
		.irq_fd = network_irq_fd,
		.read = network_read,
		.write = network_write,
	};
	const uml_backend_config config{
		.abi_version = UML_BACKEND_ABI_VERSION,
		.struct_size = sizeof(uml_backend_config),
		.rootfs_path = "/tmp/rootfs.ext2",
		.kernel_image = nullptr,
		.kernel_image_size = 0,
		.native_kernel_entry = native_kernel,
		.network = &network,
	};
	uml_backend *created = nullptr;
	require(uml_backend_create(&config, &created) == UML_BACKEND_OK,
		"backend creation failed");
	const std::unique_ptr<uml_backend, decltype(&uml_backend_destroy)> backend{
		created, uml_backend_destroy};
	uml_backend_capabilities capabilities{
		.struct_size = sizeof(uml_backend_capabilities),
	};
	require(uml_backend_get_capabilities(backend.get(), &capabilities) ==
		UML_BACKEND_OK, "capability query failed");
	require(capabilities.can_run == 1,
		"native entry was not reported as runnable");
	require(capabilities.kernel_is_native_macho == 1,
		"native entry was not reported as Mach-O");
	require(capabilities.sandbox_safe == 1,
		"native entry was not reported as sandbox safe");
	require(capabilities.userspace_execution == UML_BACKEND_NATIVE_JIT,
		"macOS did not default to the native JIT");
	const char *arguments[]{"uml-test", "--execution-engine=gadgets"};
	const char *environment[]{"UML_TEST=1", nullptr};
	require(uml_backend_run(backend.get(), 2, arguments, environment) == 73,
		"native entry result was not returned");
	require(darwin_uml_get_execution_engine() ==
		DARWIN_UML_EXECUTION_GADGETS,
		"runtime gadget selection was not applied");
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
	require(uml_backend_run(
			backend.get(), 2, invalid_arguments, environment) ==
		-UML_BACKEND_INVALID_ARGUMENT,
		"invalid execution engine was accepted");
	const char *invalid_fallback[]{
		"uml-test", "--jit-gadget-fallback=off"};
	require(uml_backend_run(
			backend.get(), 2, invalid_fallback, environment) ==
		-UML_BACKEND_INVALID_ARGUMENT,
		"removed JIT gadget fallback option was accepted");
}

#if UML_BACKEND_TEST_HAS_SLIRP
void test_default_network_requests_dhcp()
{
	const uml_backend_config config{
		.abi_version = UML_BACKEND_ABI_VERSION,
		.struct_size = sizeof(uml_backend_config),
		.rootfs_path = "/tmp/rootfs.ext2",
		.kernel_image = nullptr,
		.kernel_image_size = 0,
		.native_kernel_entry = native_dhcp_kernel,
		.network = nullptr,
	};
	uml_backend *created = nullptr;
	require(uml_backend_create(&config, &created) == UML_BACKEND_OK,
		"default-network backend creation failed");
	const std::unique_ptr<uml_backend, decltype(&uml_backend_destroy)> backend{
		created, uml_backend_destroy};
	const char *arguments[]{"uml-test"};
	require(uml_backend_run(backend.get(), 1, arguments, nullptr) == 0,
		"default-network backend run failed");
	require(dhcp_argument_present,
		"default user network did not request kernel DHCP");
}
#endif

} // namespace

int main()
{
	try {
		test_native_entry();
#if UML_BACKEND_TEST_HAS_SLIRP
		test_default_network_requests_dhcp();
#endif
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "backend API test failed: %s\n", error.what());
		return EXIT_FAILURE;
	} catch (...) {
		std::fprintf(stderr, "backend API test failed: unknown exception\n");
		return EXIT_FAILURE;
	}
}
