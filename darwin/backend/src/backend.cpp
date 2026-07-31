// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/backend.h"
#include "device.hpp"
#include "uml_darwin_runtime.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if UML_BACKEND_HAS_KERNEL_RUNNER
extern "C" {
#include "elf_loader.h"
}
#endif

#if UML_BACKEND_HAS_SLIRP
extern "C" int uml_slirp_net_open(
	void *, unsigned char[6], unsigned int *);
extern "C" int uml_slirp_net_irq_fd(void *);
extern "C" int uml_slirp_net_read(void *, void *, std::size_t);
extern "C" int uml_slirp_net_write(void *, const void *, std::size_t);
extern "C" void uml_slirp_net_stop(void);
#endif

namespace uml::backend {

class Backend final {
public:
	explicit Backend(const uml_backend_config &config)
		: rootfs_(config.rootfs_path == nullptr ? "" : config.rootfs_path),
		  native_kernel_entry_{config.native_kernel_entry},
		  network_{network_provider(config)},
		  default_user_network_{uses_default_user_network(config)}
	{
		if (config.kernel_image != nullptr && config.kernel_image_size != 0) {
			const auto image = std::span{
				static_cast<const std::byte *>(config.kernel_image),
				config.kernel_image_size};
			kernel_.assign(image.begin(), image.end());
		}
	}

	[[nodiscard]] int run(int argc, const char *const argv[],
			      const char *const envp[])
	{
		if (argc < 0 || (argc != 0 && argv == nullptr))
			return -UML_BACKEND_INVALID_ARGUMENT;
		if (native_kernel_entry_ == nullptr && kernel_.empty())
			return -UML_BACKEND_UNSUPPORTED;
		auto prepared = prepare_arguments(argc, argv);
		if (!prepared.valid || prepared.arguments.empty())
			return -UML_BACKEND_INVALID_ARGUMENT;
		if (darwin_uml_set_execution_engine(prepared.engine) != 0)
			return -UML_BACKEND_UNSUPPORTED;
		auto &arguments = prepared.arguments;
		std::vector<char *> argument_pointers;
		argument_pointers.reserve(arguments.size() + 1);
		for (auto &argument : arguments)
			argument_pointers.push_back(argument.data());
		argument_pointers.push_back(nullptr);

		std::vector<char *> environment;
		if (envp != nullptr) {
			for (auto entry = envp; *entry != nullptr; ++entry)
				environment.push_back(const_cast<char *>(*entry));
		}
		environment.push_back(nullptr);
		NetworkSession network_session{network_};

		if (native_kernel_entry_ != nullptr)
			return native_kernel_entry_(
				static_cast<int>(arguments.size()),
				argument_pointers.data(), environment.data());
#if UML_BACKEND_HAS_KERNEL_RUNNER
		return uml_elf_run(kernel_.data(), kernel_.size(),
			static_cast<int>(arguments.size()), argument_pointers.data(),
			environment.data());
#else
		return -UML_BACKEND_UNSUPPORTED;
#endif
	}

	[[nodiscard]] bool has_native_kernel() const noexcept
	{
		return native_kernel_entry_ != nullptr;
	}

	[[nodiscard]] bool has_embedded_kernel() const noexcept
	{
		return !kernel_.empty();
	}

	void request_stop() noexcept
	{
		stop_requested_.store(true, std::memory_order_release);
	}

private:
	struct PreparedArguments final {
		std::vector<std::string> arguments;
		darwin_uml_execution_engine engine{DARWIN_UML_EXECUTION_AUTO};
		bool valid{true};
	};

	class NetworkSession final {
	public:
		explicit NetworkSession(const uml::network::Provider &provider)
		{
			uml::network::activate(provider);
		}

		~NetworkSession()
		{
#if UML_BACKEND_HAS_SLIRP
			uml_slirp_net_stop();
#endif
			uml::network::deactivate();
		}

		NetworkSession(const NetworkSession &) = delete;
		NetworkSession &operator=(const NetworkSession &) = delete;
	};

	[[nodiscard]] static uml::network::Provider network_provider(
		const uml_backend_config &config) noexcept
	{
		if (config.network != nullptr)
			return {
				.context = config.network->context,
				.open = config.network->open,
				.irq_fd = config.network->irq_fd,
				.read = config.network->read,
				.write = config.network->write,
			};
#if UML_BACKEND_HAS_SLIRP
		return {
			.open = uml_slirp_net_open,
			.irq_fd = uml_slirp_net_irq_fd,
			.read = uml_slirp_net_read,
			.write = uml_slirp_net_write,
		};
#else
		return {};
#endif
	}

	[[nodiscard]] static bool uses_default_user_network(
		const uml_backend_config &config) noexcept
	{
#if UML_BACKEND_HAS_SLIRP
		if (config.network != nullptr)
			return false;
		const auto *disabled = std::getenv("UML_DISABLE_NETWORK");
		return disabled == nullptr || disabled[0] == '\0' ||
			disabled[0] == '0';
#else
		(void)config;
		return false;
#endif
	}

	[[nodiscard]] static bool has_argument(
		const std::vector<std::string> &arguments, std::string_view value)
	{
		return std::ranges::any_of(arguments, [&](const auto &argument) {
			return argument == value;
		});
	}

	[[nodiscard]] static bool has_prefix(
		const std::vector<std::string> &arguments, std::string_view prefix)
	{
		return std::ranges::any_of(arguments, [&](const auto &argument) {
			return argument.starts_with(prefix);
		});
	}

	[[nodiscard]] static bool informational(
		const std::vector<std::string> &arguments)
	{
		return has_argument(arguments, "--help") ||
			has_argument(arguments, "--version") ||
			has_argument(arguments, "--showconfig");
	}

	[[nodiscard]] static bool parse_execution_engine(
		std::string_view value, darwin_uml_execution_engine &engine)
	{
		if (value == "auto")
			engine = DARWIN_UML_EXECUTION_AUTO;
		else if (value == "jit")
			engine = DARWIN_UML_EXECUTION_JIT;
		else if (value == "gadgets")
			engine = DARWIN_UML_EXECUTION_GADGETS;
		else if (value == "interpreter")
			engine = DARWIN_UML_EXECUTION_INTERPRETER;
		else
			return false;
		return true;
	}

	[[nodiscard]] PreparedArguments prepare_arguments(
		int argc, const char *const argv[]) const
	{
		PreparedArguments prepared;
		auto &arguments = prepared.arguments;
		arguments.reserve(static_cast<std::size_t>(argc) + 6);
		for (int index = 0; index < argc; ++index) {
			if (argv[index] == nullptr)
				return {.valid = false};
			const std::string_view argument{argv[index]};
			constexpr std::string_view prefix{"--execution-engine="};
			if (argument.starts_with(prefix)) {
				if (!parse_execution_engine(
					    argument.substr(prefix.size()),
					    prepared.engine))
					return {.valid = false};
				continue;
			}
			if (argument == "--execution-engine") {
				if (++index >= argc || argv[index] == nullptr ||
				    !parse_execution_engine(
					    argv[index], prepared.engine))
					return {.valid = false};
				continue;
			}
			if (argument.starts_with("--jit-gadget-fallback"))
				return {.valid = false};
			arguments.emplace_back(argument);
		}
		if (arguments.empty())
			arguments.emplace_back("uml-darwin");
		if (informational(arguments))
			return prepared;
		if (!has_prefix(arguments, "ubd0")) {
			if (rootfs_.empty())
				return {.valid = false};
			arguments.emplace_back("ubd0=" + rootfs_);
		}
		if (!has_prefix(arguments, "root="))
			arguments.emplace_back("root=/dev/ubda");
		if (!has_prefix(arguments, "init="))
			arguments.emplace_back("init=/sbin/init");
		if (!has_prefix(arguments, "mem="))
			arguments.emplace_back("mem=256M");
		if (default_user_network_ && !has_prefix(arguments, "ip="))
			arguments.emplace_back("ip=dhcp");
		if (!has_argument(arguments, "ro") && !has_argument(arguments, "rw"))
			arguments.emplace_back("rw");
		return prepared;
	}

	std::string rootfs_;
	std::vector<std::byte> kernel_;
	uml_backend_kernel_entry native_kernel_entry_{};
	uml::network::Provider network_;
	bool default_user_network_{};
	std::atomic_bool stop_requested_{false};
};

} // namespace uml::backend

struct uml_backend {
	explicit uml_backend(const uml_backend_config &config) : implementation(config) {}

	uml::backend::Backend implementation;
};

extern "C" int uml_backend_create(const uml_backend_config *config,
				  uml_backend **result)
{
	if (config == nullptr || result == nullptr ||
	    config->abi_version != UML_BACKEND_ABI_VERSION ||
	    config->struct_size < sizeof(*config) ||
	    (config->network != nullptr &&
	     (config->network->struct_size < sizeof(*config->network) ||
	      config->network->open == nullptr ||
	      config->network->irq_fd == nullptr ||
	      config->network->read == nullptr ||
	      config->network->write == nullptr)))
		return -UML_BACKEND_INVALID_ARGUMENT;
	*result = nullptr;
	auto backend = std::unique_ptr<uml_backend>{
		new (std::nothrow) uml_backend(*config)};
	if (!backend)
		return -UML_BACKEND_OUT_OF_MEMORY;
	*result = backend.release();
	return UML_BACKEND_OK;
}

extern "C" void uml_backend_destroy(uml_backend *backend)
{
	std::unique_ptr<uml_backend>{backend};
}

extern "C" int uml_backend_get_capabilities(
	const uml_backend *backend, uml_backend_capabilities *capabilities)
{
	if (backend == nullptr || capabilities == nullptr ||
	    capabilities->struct_size < sizeof(*capabilities))
		return -UML_BACKEND_INVALID_ARGUMENT;
	capabilities->abi_version = UML_BACKEND_ABI_VERSION;
	capabilities->userspace_execution =
		static_cast<uml_backend_execution_model>(
			darwin_uml_get_execution_engine());
	const bool native_kernel = backend->implementation.has_native_kernel();
	const bool elf_kernel = UML_BACKEND_HAS_KERNEL_RUNNER &&
		backend->implementation.has_embedded_kernel();
	capabilities->can_run = native_kernel || elf_kernel;
	capabilities->kernel_is_native_macho = native_kernel;
	capabilities->sandbox_safe = native_kernel || !elf_kernel;
	capabilities->platform = UML_APPLE_PLATFORM_NAME;
	return UML_BACKEND_OK;
}

extern "C" int uml_backend_run(uml_backend *backend, int argc,
			       const char *const argv[], const char *const envp[])
{
	if (backend == nullptr)
		return -UML_BACKEND_INVALID_ARGUMENT;
	return backend->implementation.run(argc, argv, envp);
}

extern "C" void uml_backend_request_stop(uml_backend *backend)
{
	if (backend != nullptr)
		backend->implementation.request_stop();
}
