// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/in_process_service.hpp"
#include "audio/protocol.hpp"
#include "balloon/in_process_service.hpp"
#include "balloon/protocol.hpp"
#include "battery/in_process_service.hpp"
#include "battery/protocol.hpp"
#include "video/in_process_service.hpp"
#include "video/virtio_video_device.hpp"
#include "console/in_process_service.hpp"
#include "console/protocol.hpp"
#include "gpu/in_process_service.hpp"
#include "gpu/native_display_application.hpp"
#include "gpu/protocol.hpp"
#include "launcher.hpp"
#include "guest_memory_argument.hpp"
#include "sockets/socket_paths.hpp"
#include "uml/emulator.h"
#if UML_APP_HAS_SLIRP
#include "uml_slirp_net.h"
#endif

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <mach/arm/thread_status.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>

extern "C" {
extern const unsigned char uml_elf_start[]
	__asm("section$start$__DATA$__uml_elf");
extern const unsigned char uml_elf_end[]
	__asm("section$end$__DATA$__uml_elf");
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

	/* Registered before the guest brings the interface up, so this only
	 * validates the specification; a host port that cannot be claimed is
	 * reported by the provider when it installs the forward. */
	[[nodiscard]] int forward(const std::string &specification) noexcept
	{
#if UML_APP_HAS_SLIRP
		return context_ == nullptr ?
			-EINVAL :
			uml_slirp_net_add_forward(context_, specification.c_str());
#else
		(void)specification;
		return -ENOTSUP;
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
	/* This path never unwinds, so the bound socket paths would survive the
	 * process and block the next run on the same names. */
	::uml::device::remove_registered_socket_paths();
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

struct HostGpuOptions {
	bool enabled {};
	uml::gpu::DisplayMode display_mode {uml::gpu::DisplayMode::vnc};
	bool vnc_option_specified {};
	bool auto_resize_guest {true};
	bool auto_resize_guest_specified {};
	std::string vnc_bind_address {"127.0.0.1"};
	std::uint16_t vnc_port {5900};
	std::string vnc_password;
	std::uint32_t width {1280};
	std::uint32_t height {720};
	std::uint32_t dots_per_inch {};
};

struct PreparedArguments {
	HostGpuOptions gpu;
	bool audio {};
	bool battery {};
	bool video {};
	std::string console_socket;
	std::string serial_socket;
	std::vector<std::string> net_forwards;
	std::vector<std::string> kernel;
};

void append_default_kernel_argument(std::vector<std::string> &arguments,
				    std::string_view prefix,
				    std::string_view value)
{
	for (const auto &argument : arguments)
		if (argument.starts_with(prefix))
			return;
	arguments.emplace_back(value);
}

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

PreparedArguments prepare_host_arguments(
	int argc, char *const argv[],
	const uml::app::LaunchConfiguration &configuration)
{
	PreparedArguments prepared;
	prepared.kernel.reserve(static_cast<std::size_t>(argc) + 3U);
	for (int index = 0; index < argc; ++index) {
		const std::string_view argument =
			argv[index] == nullptr ? std::string_view {} : argv[index];
		if (argument == "--gpu") {
			prepared.gpu.enabled = true;
		} else if (argument == "--audio") {
			prepared.audio = true;
		} else if (argument == "--battery") {
			prepared.battery = true;
		} else if (argument == "--video") {
			prepared.video = true;
		} else if (argument.starts_with("--virtio-console-socket=")) {
			if (!prepared.console_socket.empty())
				throw std::invalid_argument(
					"--virtio-console-socket may be specified only once");
			prepared.console_socket = argument.substr(
				std::string_view{"--virtio-console-socket="}.size());
			if (prepared.console_socket.empty())
				throw std::invalid_argument(
					"--virtio-console-socket requires a path");
		} else if (argument.starts_with("--virtio-serial-socket=")) {
			if (!prepared.serial_socket.empty())
				throw std::invalid_argument(
					"--virtio-serial-socket may be specified only once");
			prepared.serial_socket = argument.substr(
				std::string_view{"--virtio-serial-socket="}.size());
			if (prepared.serial_socket.empty())
				throw std::invalid_argument(
					"--virtio-serial-socket requires a path");
		} else if (argument.starts_with("--gpu-display=")) {
			const auto mode = argument.substr(
				std::string_view{"--gpu-display="}.size());
			if (mode == "native")
				prepared.gpu.display_mode = uml::gpu::DisplayMode::native;
			else if (mode == "vnc")
				prepared.gpu.display_mode = uml::gpu::DisplayMode::vnc;
			else
				throw std::invalid_argument(
					"--gpu-display must be native or vnc");
			prepared.gpu.enabled = true;
		} else if (argument.starts_with("--gpu-vnc-bind=")) {
			prepared.gpu.vnc_option_specified = true;
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
			prepared.gpu.vnc_option_specified = true;
			const auto password =
				argument.substr(std::string_view{
					"--gpu-vnc-password="}.size());
			if (password.empty() || password.size() > 8)
				throw std::invalid_argument(
					"VNC password must contain between 1 and "
					"8 bytes");
			prepared.gpu.vnc_password = password;
			prepared.gpu.enabled = true;
		} else if (argument.starts_with("--gpu-vnc-port=")) {
			if (!parse_host_integer(argument, "--gpu-vnc-port=",
					prepared.gpu.vnc_port, true))
				throw std::invalid_argument("invalid host option: " +
					std::string(argument));
			prepared.gpu.vnc_option_specified = true;
			prepared.gpu.enabled = true;
		} else if (argument.starts_with("--gpu-auto-resize=")) {
			const auto value = argument.substr(
				std::string_view{"--gpu-auto-resize="}.size());
			if (value == "on")
				prepared.gpu.auto_resize_guest = true;
			else if (value == "off")
				prepared.gpu.auto_resize_guest = false;
			else
				throw std::invalid_argument(
					"--gpu-auto-resize must be on or off");
			prepared.gpu.auto_resize_guest_specified = true;
			prepared.gpu.enabled = true;
		} else if (argument.starts_with("--net-forward=")) {
			auto specification = std::string{argument.substr(
				std::string_view{"--net-forward="}.size())};
			if (specification.empty())
				throw std::invalid_argument(
					"--net-forward requires "
					"[PROTO:][HOSTADDR:]HOSTPORT-"
					"[GUESTADDR:]GUESTPORT");
			prepared.net_forwards.push_back(std::move(specification));
		} else if (
			   parse_host_integer(
				   argument, "--gpu-width=", prepared.gpu.width) ||
			   parse_host_integer(
				   argument, "--gpu-height=", prepared.gpu.height) ||
			   parse_host_integer(
				   argument, "--gpu-dpi=",
				   prepared.gpu.dots_per_inch)) {
			prepared.gpu.enabled = true;
		} else {
			prepared.kernel.emplace_back(argument);
		}
	}
	if (prepared.gpu.display_mode == uml::gpu::DisplayMode::native &&
	    prepared.gpu.vnc_option_specified)
		throw std::invalid_argument(
			"--gpu-vnc-* options require --gpu-display=vnc");
	if (prepared.gpu.display_mode != uml::gpu::DisplayMode::native &&
	    prepared.gpu.auto_resize_guest_specified)
		throw std::invalid_argument(
			"--gpu-auto-resize requires --gpu-display=native");
	if (prepared.gpu.dots_per_inch != 0 &&
	    (prepared.gpu.width > uml::gpu::protocol::maximumEdidDimension ||
	     prepared.gpu.height > uml::gpu::protocol::maximumEdidDimension))
		throw std::invalid_argument(
			"--gpu-dpi requires display dimensions no larger than 4095");
	if (!prepared.console_socket.empty() && !prepared.serial_socket.empty())
		throw std::invalid_argument(
			"--virtio-console-socket and --virtio-serial-socket are "
			"mutually exclusive");
	append_default_kernel_argument(
		prepared.kernel, "mem=", configuration.default_guest_memory);
	append_default_kernel_argument(
		prepared.kernel, "ncpus=", configuration.default_guest_cpus);
	if (configuration.top_byte_ignore)
		prepared.kernel.emplace_back("--guest-top-byte-ignore");
	return prepared;
}

bool has_virtio_device_argument(const std::vector<std::string> &arguments,
				std::uint32_t device_id)
{
	const std::string suffix = ":" + std::to_string(device_id);
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

} // namespace

bool uml::app::has_argument(
	int argc, char *const argv[], std::string_view argument)
{
	for (int index = 1; index < argc; ++index)
		if (argv[index] != nullptr && argument == argv[index])
			return true;
	return false;
}

bool uml::app::informational(int argc, char *const argv[])
{
	return has_argument(argc, argv, "--help") ||
		has_argument(argc, argv, "--version") ||
		has_argument(argc, argv, "--showconfig");
}

int uml::app::runEmbeddedKernel(
	int argc, char *argv[], char *envp[],
	const LaunchConfiguration &configuration)
{
	install_host_faults();
	::uml::device::install_socket_path_signal_cleanup();
	PreparedArguments prepared;
	try {
		prepared = prepare_host_arguments(argc, argv, configuration);
	} catch (const std::exception &error) {
		std::fprintf(stderr, "Unable to configure host devices: %s\n",
			     error.what());
		return 1;
	}
	const bool information_only = informational(argc, argv);
	if (!information_only && !host_has_lse_atomics()) {
		std::fputs("This Darwin UML kernel requires Armv8.1 atomic "
			   "instructions on the host\n", stderr);
		return 1;
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
		for (const auto &specification : prepared.net_forwards) {
			if (app_network->forward(specification) == 0)
				continue;
			std::fprintf(stderr,
				"Unable to accept --net-forward=%s\n",
				specification.c_str());
			return 1;
		}
	} else if (!prepared.net_forwards.empty()) {
		std::fputs("--net-forward requires the user network\n", stderr);
		return 1;
	}

	const uml_emulator_config config{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = configuration.rootfs_path,
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
	/* The guest's host-visible shared-memory region belongs to the GPU
	 * backend, and the video decoder fills capture buffers that live in it,
	 * so both backends share one index of the ranges published there. */
	auto shared_memory_regions =
		std::make_shared<uml::virtio::SharedMemoryRegions>();
	/* GPU memory is charged to the process like guest RAM, so the GPU may
	 * use at most half of the guest's allocation and the balloon takes what
	 * it uses out of the guest. */
	const std::uint64_t guest_memory =
		uml::emulator::configured_guest_memory_bytes(prepared.kernel);
	const std::uint64_t device_memory_limit = guest_memory / 2;
	if (prepared.gpu.enabled && !information_only) {
		if (has_virtio_device_argument(
			prepared.kernel, uml::gpu::protocol::virtioGpuDeviceId)) {
			std::fputs("--gpu cannot be combined with an explicit "
				   "Virtio-GPU transport argument\n",
				   stderr);
			return 1;
		}
		try {
			gpu_service =
				std::make_unique<uml::gpu::InProcessGpuService>(
					uml::gpu::InProcessGpuOptions{
						.displayMode = prepared.gpu.display_mode,
						.vncBindAddress =
							prepared.gpu.vnc_bind_address,
						.vncPort = prepared.gpu.vnc_port,
						.vncPassword =
							prepared.gpu.vnc_password,
						.width = prepared.gpu.width,
						.height = prepared.gpu.height,
						.dotsPerInch =
							prepared.gpu.dots_per_inch,
						.pointerMode = configuration.input_mode != DisplayInput::mouse ?
							uml::input::vnc::PointerMode::touchscreen :
							uml::input::vnc::PointerMode::mouse,
						.autoResizeGuest =
							prepared.gpu.auto_resize_guest,
						.sharedMemoryRegions =
							shared_memory_regions,
						.deviceMemoryLimit =
							device_memory_limit,
					});
			prepared.kernel.emplace_back(
				"virtio_uml.device=" +
				gpu_service->takeConnectionName() + ":" +
				std::to_string(
					uml::gpu::protocol::virtioGpuDeviceId));
			prepared.kernel.emplace_back(
				"uml_vnc_input." + gpu_service->takeInputConnectionName());
		} catch (const std::exception &error) {
			std::fprintf(stderr,
				"Unable to start in-process Virtio-GPU: %s\n",
				error.what());
			return 1;
		}
	}
	std::unique_ptr<uml::battery::InProcessBatteryService> battery_service;
	if (prepared.battery && !information_only) {
		if (has_virtio_device_argument(
			prepared.kernel, uml::battery::protocol::deviceId)) {
			std::fputs("--battery cannot be combined with an explicit "
				   "Virtio battery transport argument\n",
				   stderr);
			return 1;
		}
		try {
			battery_service =
				std::make_unique<uml::battery::InProcessBatteryService>();
			/* Android's vendor policy labels the battery's sysfs
			 * path by its position among the Virtio devices
			 * (android/aosp-17/overlay/sepolicy/vendor/genfs_contexts).
			 * Straight after the GPU it is always virtio-uml.1/virtio1,
			 * whatever optional devices follow. */
			prepared.kernel.emplace_back(
				"virtio_uml.device=" +
				battery_service->takeConnectionName() + ":" +
				std::to_string(uml::battery::protocol::deviceId));
		} catch (const std::exception &error) {
			std::fprintf(stderr,
				"Unable to start in-process Virtio battery: %s\n",
				error.what());
			return 1;
		}
	}
	std::unique_ptr<uml::balloon::InProcessBalloonService> balloon_service;
	if (gpu_service && device_memory_limit != 0 &&
	    !has_virtio_device_argument(
		    prepared.kernel, uml::balloon::protocol::deviceId)) {
		try {
			balloon_service =
				std::make_unique<uml::balloon::InProcessBalloonService>(
					uml::balloon::BalloonOptions{
						.guestMemoryBytes = guest_memory,
						.target = uml::balloon::hostGpuMemoryTarget(
							device_memory_limit),
						.hostPageSize = static_cast<std::size_t>(
							getpagesize()),
					});
			prepared.kernel.emplace_back(
				"virtio_uml.device=" +
				balloon_service->takeConnectionName() + ":" +
				std::to_string(uml::balloon::protocol::deviceId));
		} catch (const std::exception &error) {
			std::fprintf(stderr,
				"Unable to start in-process Virtio balloon: %s\n",
				error.what());
			return 1;
		}
	}
	std::unique_ptr<uml::video::InProcessVideoService> video_service;
	if (prepared.video && !information_only) {
		if (has_virtio_device_argument(prepared.kernel, uml::video::deviceId)) {
			std::fputs("--video cannot be combined with an explicit Virtio video transport argument\n", stderr);
			return 1;
		}
		try {
			video_service = std::make_unique<uml::video::InProcessVideoService>(
				gpu_service ? shared_memory_regions : nullptr);
			prepared.kernel.emplace_back("virtio_uml.device=" +
				video_service->takeConnectionName() + ":" + std::to_string(uml::video::deviceId));
		} catch (const std::exception& error) {
			std::fprintf(stderr, "Unable to start in-process Virtio video: %s\n", error.what());
			return 1;
		}
	}
	std::unique_ptr<uml::audio::InProcessAudioService> audio_service;
	if (prepared.audio && !information_only) {
		if (has_virtio_device_argument(
			prepared.kernel, uml::audio::protocol::deviceId)) {
			std::fputs("--audio cannot be combined with an explicit "
				   "Virtio Sound transport argument\n",
				   stderr);
			return 1;
		}
		try {
			audio_service =
				std::make_unique<uml::audio::InProcessAudioService>();
			prepared.kernel.emplace_back(
				"virtio_uml.device=" +
				audio_service->takeConnectionName() + ":" +
				std::to_string(uml::audio::protocol::deviceId));
		} catch (const std::exception &error) {
			std::fprintf(stderr,
				"Unable to start in-process Virtio Sound: %s\n",
				error.what());
			return 1;
		}
	}
	std::unique_ptr<uml::console::InProcessConsoleService> console_service;
	if ((!prepared.console_socket.empty() || !prepared.serial_socket.empty()) &&
	    !information_only) {
		const bool named_serial = !prepared.serial_socket.empty();
		if (has_virtio_device_argument(
			prepared.kernel, uml::console::protocol::deviceId)) {
			std::fputs("a host Virtio console or serial socket cannot be "
				   "combined with an explicit Virtio console transport "
				   "argument\n",
				   stderr);
			return 1;
		}
		try {
			console_service =
				std::make_unique<uml::console::InProcessConsoleService>(
					named_serial ? prepared.serial_socket :
						prepared.console_socket,
					named_serial ?
						std::string(
							uml::console::protocol::hostSerialPortName) :
						std::string {});
			prepared.kernel.emplace_back(
				"virtio_uml.device=" +
				console_service->takeConnectionName() + ":" +
				std::to_string(uml::console::protocol::deviceId));
		} catch (const std::exception &error) {
			std::fprintf(
				stderr, "Unable to start in-process Virtio %s: %s\n",
				named_serial ? "serial" : "console",
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
	const auto run = [&] {
		return uml_emulator_run(
			emulator.get(), static_cast<int>(arguments.size()),
			arguments.data(), environment.data());
	};
	const int result = prepared.gpu.enabled &&
		prepared.gpu.display_mode == uml::gpu::DisplayMode::native ?
		uml::gpu::runNativeDisplayApplication(
			run, [&] { uml_emulator_request_stop(emulator.get()); }) :
		run();
	if (app_network)
		app_network->shutdown();
	if (audio_service)
		audio_service->stop();
	if (battery_service)
		battery_service->stop();
	if (video_service)
		video_service->stop();
	if (console_service)
		console_service->stop();
	if (gpu_service)
		gpu_service->stop();
	if (!configuration.run_scratch.empty()) {
		std::error_code ignored;
		std::filesystem::remove_all(configuration.run_scratch, ignored);
	}
	/* The loader hands the kernel's signal state back to the host before
	 * uml_elf_run returns, so no Linux handler is left behind. The launcher
	 * still leaves through _Exit, which runs no static destructor or atexit
	 * handler; code that must write at exit, such as the guest-code
	 * sampler, does so before this point.
	 */
	std::_Exit(result);
}
