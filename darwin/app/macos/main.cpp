// SPDX-License-Identifier: GPL-2.0-or-later
#include "in_process_service.hpp"
#include "link_profile_api.hpp"
#include "protocol.hpp"
#include "uml/emulator.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <mach/arm/thread_status.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>

extern "C" {
extern const unsigned char uml_elf_start[]
	__asm("section$start$__DATA$__uml_elf");
extern const unsigned char uml_elf_end[]
	__asm("section$end$__DATA$__uml_elf");
#if UML_APP_HAS_SLIRP
int uml_slirp_net_open(void *, unsigned char[6], unsigned int *);
int uml_slirp_net_irq_fd(void *);
int uml_slirp_net_read(void *, void *, std::size_t);
int uml_slirp_net_write(void *, const void *, std::size_t);
void uml_slirp_net_stop(void *);
void *uml_slirp_net_create(void) noexcept;
void uml_slirp_net_destroy(void *) noexcept;
#endif
}

namespace {

constexpr char host_fault_prefix[] = "Darwin UML host signal ";
constexpr char host_fault_pc_prefix[] = " at pc=0x";
constexpr char host_fault_address_prefix[] = " address=0x";
constexpr std::size_t host_fault_hex_digits = sizeof(std::uintptr_t) * 2U;
constexpr std::size_t host_fault_message_capacity = 128;

class AppNetwork final {
public:
	AppNetwork()
	{
#if UML_APP_HAS_SLIRP
		context_ = uml_slirp_net_create();
		if (context_ == nullptr)
			throw std::bad_alloc{};
#endif
	}

	~AppNetwork() { shutdown(); }

	AppNetwork(const AppNetwork &) = delete;
	AppNetwork &operator=(const AppNetwork &) = delete;

	[[nodiscard]] uml_emulator_network_callbacks callbacks() noexcept
	{
#if UML_APP_HAS_SLIRP
		return {
			.struct_size = sizeof(uml_emulator_network_callbacks),
			.context = context_,
			.open = uml_slirp_net_open,
			.irq_fd = uml_slirp_net_irq_fd,
			.read = uml_slirp_net_read,
			.write = uml_slirp_net_write,
		};
#else
		return {};
#endif
	}

	void shutdown() noexcept
	{
#if UML_APP_HAS_SLIRP
		if (context_ != nullptr) {
			uml_slirp_net_stop(context_);
			uml_slirp_net_destroy(context_);
			context_ = nullptr;
		}
#endif
	}

private:
	void *context_{};
};

[[nodiscard]] bool user_network_enabled() noexcept
{
#if UML_APP_HAS_SLIRP
	const auto *disabled = std::getenv("UML_DISABLE_NETWORK");
	return disabled == nullptr || disabled[0] == '\0' ||
		disabled[0] == '0';
#else
	return false;
#endif
}

template<std::size_t Size>
char *append_host_fault_literal(char *output,
				const char (&literal)[Size]) noexcept
{
	for (std::size_t index = 0; index + 1U < Size; ++index)
		*output++ = literal[index];
	return output;
}

char *append_host_fault_signal(char *output, int signal) noexcept
{
	constexpr std::size_t decimal_digits =
		std::numeric_limits<unsigned int>::digits10 + 1U;
	char reversed[decimal_digits];
	std::size_t count = 0;
	unsigned int magnitude = static_cast<unsigned int>(signal);
	if (signal < 0) {
		*output++ = '-';
		magnitude = 0U - magnitude;
	}
	do {
		reversed[count++] = static_cast<char>('0' + magnitude % 10U);
		magnitude /= 10U;
	} while (magnitude != 0U);
	while (count != 0U)
		*output++ = reversed[--count];
	return output;
}

char *append_host_fault_hex(char *output, std::uintptr_t value) noexcept
{
	constexpr char digits[] = "0123456789abcdef";
	for (std::size_t index = host_fault_hex_digits; index != 0U; --index) {
		const auto shift = (index - 1U) * 4U;
		*output++ = digits[(value >> shift) & 0xfU];
	}
	return output;
}

void host_fault(int signal, siginfo_t *info, void *context_pointer) noexcept
{
	auto &context = *static_cast<ucontext_t *>(context_pointer);
	const auto pc = static_cast<std::uintptr_t>(
		__darwin_arm_thread_state64_get_pc(context.uc_mcontext->__ss));
	const auto address = reinterpret_cast<std::uintptr_t>(info->si_addr);
	static_assert((sizeof(host_fault_prefix) - 1U) +
			      (std::numeric_limits<unsigned int>::digits10 + 2U) +
			      (sizeof(host_fault_pc_prefix) - 1U) +
			      host_fault_hex_digits +
			      (sizeof(host_fault_address_prefix) - 1U) +
			      host_fault_hex_digits + 1U <=
		      host_fault_message_capacity);
	char message[host_fault_message_capacity];
	char *output = message;
	output = append_host_fault_literal(output, host_fault_prefix);
	output = append_host_fault_signal(output, signal);
	output = append_host_fault_literal(output, host_fault_pc_prefix);
	output = append_host_fault_hex(output, pc);
	output = append_host_fault_literal(output, host_fault_address_prefix);
	output = append_host_fault_hex(output, address);
	*output++ = '\n';
	(void)write(STDERR_FILENO, message,
		    static_cast<std::size_t>(output - message));
	_exit(128 + signal);
}

void install_host_faults()
{
	constexpr std::array signals{SIGABRT, SIGBUS, SIGSEGV, SIGILL};
	struct sigaction action {};
	action.sa_sigaction = host_fault;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	for (const int signal : signals)
		if (sigaction(signal, &action, nullptr) < 0)
			std::abort();
}

bool has_argument(int argc, char *const argv[], std::string_view argument)
{
	for (int index = 1; index < argc; ++index)
		if (argv[index] != nullptr && argument == argv[index])
			return true;
	return false;
}

bool has_prefix(int argc, char *const argv[], std::string_view prefix)
{
	for (int index = 1; index < argc; ++index)
		if (argv[index] != nullptr &&
		    std::string_view{argv[index]}.starts_with(prefix))
			return true;
	return false;
}

bool informational(int argc, char *const argv[])
{
	return has_argument(argc, argv, "--help") ||
		has_argument(argc, argv, "--version") ||
		has_argument(argc, argv, "--showconfig");
}

struct HostGpuOptions {
	bool enabled {};
	std::string vnc_bind_address {"127.0.0.1"};
	std::uint16_t vnc_port {5900};
	std::string vnc_password;
	std::uint32_t width {1280};
	std::uint32_t height {720};
};

struct PreparedArguments {
	HostGpuOptions gpu;
	std::vector<std::string> kernel;
};

template<typename Integer>
bool parse_host_integer(std::string_view argument, std::string_view prefix,
			Integer &destination, bool allow_zero = false)
{
	if (!argument.starts_with(prefix))
		return false;
	const auto value = argument.substr(prefix.size());
	Integer parsed {};
	const auto [end, error] = std::from_chars(
		value.data(), value.data() + value.size(), parsed);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    (!allow_zero && parsed == 0))
		throw std::invalid_argument("invalid host option: " +
					    std::string(argument));
	destination = parsed;
	return true;
}

PreparedArguments prepare_host_arguments(int argc, char *const argv[])
{
	PreparedArguments prepared;
	prepared.kernel.reserve(static_cast<std::size_t>(argc) + 1U);
	for (int index = 0; index < argc; ++index) {
		const std::string_view argument =
			argv[index] == nullptr ? std::string_view {} : argv[index];
		if (argument == "--gpu") {
			prepared.gpu.enabled = true;
		} else if (argument.starts_with("--gpu-vnc-bind=")) {
			const auto address =
				argument.substr(std::string_view{
					"--gpu-vnc-bind="}.size());
			if (address.empty())
				throw std::invalid_argument(
					"invalid host option: " +
					std::string(argument));
			prepared.gpu.vnc_bind_address = address;
			prepared.gpu.enabled = true;
		} else if (argument.starts_with("--gpu-vnc-password=")) {
			const auto password =
				argument.substr(std::string_view{
					"--gpu-vnc-password="}.size());
			if (password.empty() || password.size() > 8)
				throw std::invalid_argument(
					"VNC password must contain between 1 and "
					"8 bytes");
			prepared.gpu.vnc_password = password;
			prepared.gpu.enabled = true;
		} else if (parse_host_integer(
				   argument, "--gpu-vnc-port=",
				   prepared.gpu.vnc_port, true) ||
			   parse_host_integer(
				   argument, "--gpu-width=", prepared.gpu.width) ||
			   parse_host_integer(
				   argument, "--gpu-height=", prepared.gpu.height)) {
			prepared.gpu.enabled = true;
		} else {
			prepared.kernel.emplace_back(argument);
		}
	}
	return prepared;
}

bool has_gpu_device_argument(const std::vector<std::string> &arguments)
{
	const std::string suffix =
		":" + std::to_string(uml::gpu::protocol::virtioGpuDeviceId);
	for (const auto &argument : arguments)
		if (argument.starts_with("virtio_uml.device=") &&
		    argument.ends_with(suffix))
			return true;
	return false;
}

bool host_has_lse_atomics()
{
	int supported = 0;
	std::size_t size = sizeof(supported);
	return sysctlbyname("hw.optional.arm.FEAT_LSE", &supported, &size,
			    nullptr, 0) == 0 &&
		size == sizeof(supported) && supported != 0;
}

bool usable_rootfs(const std::filesystem::path &path)
{
	struct stat details {};
	return stat(path.c_str(), &details) == 0 && S_ISREG(details.st_mode) &&
		(details.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0 &&
		access(path.c_str(), R_OK) == 0;
}

std::optional<std::filesystem::path> canonical_rootfs(
	const std::filesystem::path &candidate)
{
	std::error_code error;
	auto path = std::filesystem::canonical(candidate, error);
	if (!error && usable_rootfs(path))
		return path;
	return std::nullopt;
}

std::optional<std::filesystem::path> locate_rootfs()
{
	if (const char *configured = std::getenv("UML_ROOTFS");
	    configured != nullptr && configured[0] != '\0') {
		auto path = canonical_rootfs(configured);
		if (!path)
			std::fprintf(stderr,
				"UML_ROOTFS is not a readable immutable rootfs image: %s\n",
				configured);
		return path;
	}

	std::array<char, 4096> executable{};
	std::uint32_t length = executable.size();
	if (_NSGetExecutablePath(executable.data(), &length) == 0) {
		std::error_code error;
		auto parent = std::filesystem::canonical(executable.data(), error)
			.parent_path();
		if (!error)
			if (auto path = canonical_rootfs(parent / "rootfs.img"))
				return path;
	}
	if (auto path = canonical_rootfs("rootfs.img"))
		return path;
	std::fputs("rootfs.img was not found beside the executable or in the "
		"current directory\n", stderr);
	return std::nullopt;
}

} // namespace

int main(int argc, char *argv[], char *envp[])
{
	install_host_faults();
	PreparedArguments prepared;
	try {
		prepared = prepare_host_arguments(argc, argv);
	} catch (const std::exception &error) {
		std::fprintf(stderr, "Unable to configure in-process GPU: %s\n",
			     error.what());
		return 1;
	}
	if (!informational(argc, argv) && !host_has_lse_atomics()) {
		std::fputs("This Darwin UML kernel requires Armv8.1 atomic "
			   "instructions on the host\n", stderr);
		return 1;
	}
	std::string rootfs;
	if (!informational(argc, argv) && !has_prefix(argc, argv, "ubd0")) {
		auto located = locate_rootfs();
		if (!located)
			return 1;
		rootfs = located->string();
	}

	std::unique_ptr<AppNetwork> app_network;
	uml_emulator_network_callbacks network_callbacks{};
	if (user_network_enabled()) {
		try {
			app_network = std::make_unique<AppNetwork>();
			network_callbacks = app_network->callbacks();
		} catch (const std::exception &error) {
			std::fprintf(stderr, "Unable to create user network: %s\n",
				error.what());
			return 1;
		}
	}

	const uml_emulator_config config{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = rootfs.empty() ? nullptr : rootfs.c_str(),
		.kernel_image = uml_elf_start,
		.kernel_image_size = static_cast<std::size_t>(
			uml_elf_end - uml_elf_start),
		.native_kernel_entry = nullptr,
		.network = app_network ? &network_callbacks : nullptr,
	};
	uml_emulator *created = nullptr;
	const int create_result = uml_emulator_create(&config, &created);
	if (create_result != UML_EMULATOR_OK) {
		std::fprintf(stderr, "Unable to create UML emulator: %d\n",
			create_result);
		return 1;
	}
	const std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)> emulator{
		created, uml_emulator_destroy};
	std::unique_ptr<uml::gpu::InProcessGpuService> gpu_service;
	if (prepared.gpu.enabled && !informational(argc, argv)) {
		if (has_gpu_device_argument(prepared.kernel)) {
			std::fputs("--gpu cannot be combined with an explicit "
				   "Virtio-GPU transport argument\n",
				   stderr);
			return 1;
		}
		try {
			gpu_service =
				std::make_unique<uml::gpu::InProcessGpuService>(
					uml::gpu::InProcessGpuOptions{
						.vncBindAddress =
							prepared.gpu.vnc_bind_address,
						.vncPort = prepared.gpu.vnc_port,
						.vncPassword =
							prepared.gpu.vnc_password,
						.width = prepared.gpu.width,
						.height = prepared.gpu.height,
					});
			prepared.kernel.emplace_back(
				"virtio_uml.device=" +
				gpu_service->takeConnectionName() + ":" +
				std::to_string(
					uml::gpu::protocol::virtioGpuDeviceId));
			prepared.kernel.emplace_back(
				"uml_vnc_input." +
				gpu_service->takeInputConnectionName());
		} catch (const std::exception &error) {
			std::fprintf(stderr,
				"Unable to start in-process Virtio-GPU: %s\n",
				error.what());
			return 1;
		}
	}
	std::vector<const char *> arguments;
	arguments.reserve(prepared.kernel.size());
	for (const auto &argument : prepared.kernel)
		arguments.push_back(argument.c_str());
	std::vector<const char *> environment;
	for (auto entry = envp; entry != nullptr && *entry != nullptr; ++entry)
		environment.push_back(*entry);
	environment.push_back(nullptr);
	const int result = uml_emulator_run(
		emulator.get(), static_cast<int>(arguments.size()), arguments.data(),
		environment.data());
	if (app_network)
		app_network->shutdown();
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
	uml_emulator_report_jit_gpr_link_profile();
#endif
	if (gpu_service)
		gpu_service->stop();
	/* The embedded ELF kernel installs process-wide Linux signal handlers.
	 * Once it halts, avoid running unrelated C++ static teardown through that
	 * signal environment.  Native app-linked kernels can use normal app
	 * lifecycle management.
	 */
	std::_Exit(result);
}
