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
#include "gpu/metal_shader_cache.hpp"
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

/* Metal reads the whole index of the process's shader cache at its first
 * compile, and conformance campaigns grow that index without bound. Cache the
 * guest's shaders only while the index is small; a removal is the only event
 * worth a line. */
void bound_metal_shader_cache()
{
	std::error_code error;
	const auto outcome = uml::gpu::bound_own_metal_shader_cache(error);
	if (!outcome)
		return;
	if (error) {
		std::fprintf(stderr,
			"Unable to remove the Metal shader cache %s: %s\n",
			outcome->directory.c_str(), error.message().c_str());
		return;
	}
	if (outcome->action ==
	    uml::gpu::MetalShaderCacheOutcome::Action::removed)
		std::fprintf(stderr,
			"Removed the Metal shader cache %s: index %.1f MB over "
			"%.1f MB, data %.1f MB\n",
			outcome->directory.c_str(),
			static_cast<double>(outcome->size.index) / 1e6,
			static_cast<double>(
				uml::gpu::metal_shader_cache_index_limit) /
				1e6,
			static_cast<double>(outcome->size.data) / 1e6);
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
	uml::audio::OutputMode audio_output {uml::audio::OutputMode::host};
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

/* Records the path of a socket option, which may be given once. */
void set_socket_path(std::string &destination, std::string_view option,
		     std::string_view value)
{
	if (!destination.empty())
		throw std::invalid_argument(std::string(option) +
					    " may be specified only once");
	if (value.empty())
		throw std::invalid_argument(std::string(option) +
					    " requires a path");
	destination = value;
}

PreparedArguments prepare_host_arguments(
	const std::vector<std::string> &command_line,
	const uml::app::LaunchConfiguration &configuration)
{
	PreparedArguments prepared;
	prepared.kernel.reserve(command_line.size() + 3U);
	for (const auto &text : command_line) {
		const std::string_view argument = text;
		/* Whether `argument` is the option `prefix` names ("--x="), with
		 * what follows it left in `value`. */
		std::string_view value;
		const auto valued = [&](std::string_view prefix) {
			if (!argument.starts_with(prefix))
				return false;
			value = argument.substr(prefix.size());
			return true;
		};
		if (argument == "--gpu") {
			prepared.gpu.enabled = true;
		} else if (argument == "--audio") {
			prepared.audio = true;
		} else if (valued("--audio-output=")) {
			if (value == "host")
				prepared.audio_output = uml::audio::OutputMode::host;
			else if (value == "null")
				prepared.audio_output = uml::audio::OutputMode::null;
			else
				throw std::invalid_argument(
					"--audio-output must be host or null");
			prepared.audio = true;
		} else if (argument == "--battery") {
			prepared.battery = true;
		} else if (argument == "--video") {
			prepared.video = true;
		} else if (valued("--virtio-console-socket=")) {
			set_socket_path(prepared.console_socket,
					"--virtio-console-socket", value);
		} else if (valued("--virtio-serial-socket=")) {
			set_socket_path(prepared.serial_socket,
					"--virtio-serial-socket", value);
		} else if (valued("--gpu-display=")) {
			if (value == "native")
				prepared.gpu.display_mode = uml::gpu::DisplayMode::native;
			else if (value == "vnc")
				prepared.gpu.display_mode = uml::gpu::DisplayMode::vnc;
			else
				throw std::invalid_argument(
					"--gpu-display must be native or vnc");
			prepared.gpu.enabled = true;
		} else if (valued("--gpu-vnc-bind=")) {
			prepared.gpu.vnc_option_specified = true;
			if (value.empty())
				throw std::invalid_argument(
					"invalid host option: " +
					std::string(argument));
			prepared.gpu.vnc_bind_address = value;
			prepared.gpu.enabled = true;
		} else if (valued("--gpu-vnc-password=")) {
			prepared.gpu.vnc_option_specified = true;
			if (value.empty() || value.size() > 8)
				throw std::invalid_argument(
					"VNC password must contain between 1 and "
					"8 bytes");
			prepared.gpu.vnc_password = value;
			prepared.gpu.enabled = true;
		} else if (parse_host_integer(argument, "--gpu-vnc-port=",
					      prepared.gpu.vnc_port, true)) {
			prepared.gpu.vnc_option_specified = true;
			prepared.gpu.enabled = true;
		} else if (valued("--gpu-auto-resize=")) {
			if (value == "on")
				prepared.gpu.auto_resize_guest = true;
			else if (value == "off")
				prepared.gpu.auto_resize_guest = false;
			else
				throw std::invalid_argument(
					"--gpu-auto-resize must be on or off");
			prepared.gpu.auto_resize_guest_specified = true;
			prepared.gpu.enabled = true;
		} else if (valued("--net-forward=")) {
			if (value.empty())
				throw std::invalid_argument(
					"--net-forward requires "
					"[PROTO:][HOSTADDR:]HOSTPORT-"
					"[GUESTADDR:]GUESTPORT");
			prepared.net_forwards.emplace_back(value);
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
			prepared.kernel.push_back(text);
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

/* Starts the in-process Virtio device that `make` creates and appends the
 * kernel argument that hands it to the guest, unless the command line already
 * names a transport for that device. A refusal or a start failure prints its
 * reason and returns false: `subject` names what asked for the device,
 * `device` what failed to start and `transport` (`device` when null) what the
 * explicit argument would collide with. */
template<typename Make>
[[nodiscard]] bool start_virtio_device(
	std::vector<std::string> &kernel,
	std::unique_ptr<uml::gpu::InProcessVhostUserService> &service,
	std::uint32_t device_id, Make make, const char *subject,
	const char *device, const char *transport = nullptr)
{
	if (has_virtio_device_argument(kernel, device_id)) {
		std::fprintf(stderr,
			"%s cannot be combined with an explicit %s transport "
			"argument\n",
			subject, transport != nullptr ? transport : device);
		return false;
	}
	try {
		service = make();
		kernel.emplace_back(uml::gpu::virtioDeviceArgument(
			service->takeConnectionName(), device_id));
	} catch (const std::exception &error) {
		std::fprintf(stderr, "Unable to start in-process %s: %s\n",
			device, error.what());
		return false;
	}
	return true;
}

/* Whether `argument` asks for a run that only prints information and boots
 * no guest. */
bool informational_option(std::string_view argument)
{
	return argument == "--help" || argument == "--version" ||
		argument == "--showconfig";
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
	for (int index = 1; index < argc; ++index)
		if (argv[index] != nullptr && informational_option(argv[index]))
			return true;
	return false;
}

int uml::app::runEmbeddedKernel(
	const std::vector<std::string> &command_line, char *envp[],
	const LaunchConfiguration &configuration)
{
	install_host_faults();
	::uml::device::install_socket_path_signal_cleanup();
	PreparedArguments prepared;
	try {
		prepared = prepare_host_arguments(command_line, configuration);
	} catch (const std::exception &error) {
		std::fprintf(stderr, "Unable to configure host devices: %s\n",
			     error.what());
		return 1;
	}
	bool information_only = false;
	for (std::size_t index = 1; index < command_line.size(); ++index)
		information_only |= informational_option(command_line[index]);
	/* Before anything in the process can create a Metal device. */
	if (prepared.gpu.enabled && !information_only)
		bound_metal_shader_cache();
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
				uml::gpu::virtioDeviceArgument(
					gpu_service->takeConnectionName(),
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
	std::unique_ptr<uml::gpu::InProcessVhostUserService> battery_service;
	/* Android's vendor policy labels the battery's sysfs path by its
	 * position among the Virtio devices
	 * (android/lineage-24/overlay/sepolicy/vendor/genfs_contexts).
	 * Straight after the GPU it is always virtio-uml.1/virtio1, whatever
	 * optional devices follow. */
	if (prepared.battery && !information_only &&
	    !start_virtio_device(
		    prepared.kernel, battery_service,
		    uml::battery::protocol::deviceId,
		    [] { return uml::battery::makeInProcessService(); },
		    "--battery", "Virtio battery"))
		return 1;
	std::unique_ptr<uml::gpu::InProcessVhostUserService> balloon_service;
	if (gpu_service && device_memory_limit != 0 &&
	    !has_virtio_device_argument(
		    prepared.kernel, uml::balloon::protocol::deviceId)) {
		try {
			balloon_service = uml::balloon::makeInProcessService(
				uml::balloon::BalloonOptions{
					.guestMemoryBytes = guest_memory,
					.target = uml::balloon::hostGpuMemoryTarget(
						device_memory_limit),
					.hostPageSize = static_cast<std::size_t>(
						getpagesize()),
				});
			prepared.kernel.emplace_back(
				uml::gpu::virtioDeviceArgument(
					balloon_service->takeConnectionName(),
					uml::balloon::protocol::deviceId));
		} catch (const std::exception &error) {
			std::fprintf(stderr,
				"Unable to start in-process Virtio balloon: %s\n",
				error.what());
			return 1;
		}
	}
	std::unique_ptr<uml::gpu::InProcessVhostUserService> video_service;
	if (prepared.video && !information_only &&
	    !start_virtio_device(
		    prepared.kernel, video_service, uml::video::deviceId,
		    [&] {
			    return uml::video::makeInProcessService(
				    gpu_service ? shared_memory_regions : nullptr);
		    },
		    "--video", "Virtio video"))
		return 1;
	std::unique_ptr<uml::gpu::InProcessVhostUserService> audio_service;
	if (prepared.audio && !information_only &&
	    !start_virtio_device(
		    prepared.kernel, audio_service,
		    uml::audio::protocol::deviceId,
		    [&] {
			    return uml::audio::makeInProcessService(
				    {.output = prepared.audio_output});
		    },
		    "--audio", "Virtio Sound"))
		return 1;
	std::unique_ptr<uml::gpu::InProcessVhostUserService> console_service;
	const bool named_serial = !prepared.serial_socket.empty();
	if ((named_serial || !prepared.console_socket.empty()) &&
	    !information_only &&
	    !start_virtio_device(
		    prepared.kernel, console_service,
		    uml::console::protocol::deviceId,
		    [&] {
			    return named_serial ?
				    uml::console::makeInProcessService(
					    prepared.serial_socket,
					    std::string(uml::console::protocol::
							hostSerialPortName)) :
				    uml::console::makeInProcessService(
					    prepared.console_socket);
		    },
		    "a host Virtio console or serial socket",
		    named_serial ? "Virtio serial" : "Virtio console",
		    "Virtio console"))
		return 1;
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
	if (balloon_service)
		balloon_service->stop();
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
