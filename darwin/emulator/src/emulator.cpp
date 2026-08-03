// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/emulator.h"
#include "device.hpp"
#include "options.hpp"
#include "context.hpp"
#include "uml_darwin_cpu.h"

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if UML_EMULATOR_HAS_KERNEL_RUNNER
extern "C" {
#include "elf_loader.h"
}
#endif

#if UML_EMULATOR_HAS_SLIRP
extern "C" int uml_slirp_net_open(
	void *, unsigned char[6], unsigned int *);
extern "C" int uml_slirp_net_irq_fd(void *);
extern "C" int uml_slirp_net_read(void *, void *, std::size_t);
extern "C" int uml_slirp_net_write(void *, const void *, std::size_t);
extern "C" void uml_slirp_net_stop(void *);
extern "C" void *uml_slirp_net_create(void) noexcept;
extern "C" void uml_slirp_net_destroy(void *) noexcept;
#endif

namespace uml::emulator {

#if UML_EMULATOR_HAS_KERNEL_RUNNER
namespace {
std::mutex embedded_kernel_run_mutex;
}
#endif

class DefaultNetwork final {
public:
	explicit DefaultNetwork(bool enabled)
	{
#if UML_EMULATOR_HAS_SLIRP
		if (enabled) {
			context_ = uml_slirp_net_create();
			if (context_ == nullptr)
				throw std::bad_alloc{};
		}
#else
		(void)enabled;
#endif
	}

	~DefaultNetwork()
	{
#if UML_EMULATOR_HAS_SLIRP
		uml_slirp_net_destroy(context_);
#endif
	}

	DefaultNetwork(const DefaultNetwork &) = delete;
	DefaultNetwork &operator=(const DefaultNetwork &) = delete;

	[[nodiscard]] void *get() const noexcept { return context_; }

private:
	void *context_{};
};

class Emulator final {
public:
	explicit Emulator(const uml_emulator_config &config)
		: rootfs_(config.rootfs_path == nullptr ? "" : config.rootfs_path),
		  native_kernel_entry_{config.native_kernel_entry},
		  runtime_{},
		  default_network_{uses_default_user_network(config)},
		  network_{network_provider(config, default_network_.get())},
		  network_context_{network_},
		  default_user_network_{default_network_.get() != nullptr}
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
			return -UML_EMULATOR_INVALID_ARGUMENT;
		if (native_kernel_entry_ == nullptr && kernel_.empty())
			return -UML_EMULATOR_UNSUPPORTED;
		auto prepared = prepare_emulator_options(
			argc, argv, rootfs_, default_user_network_);
		if (!prepared.valid || prepared.arguments.empty())
			return -UML_EMULATOR_INVALID_ARGUMENT;
#if UML_EMULATOR_HAS_KERNEL_RUNNER
		std::unique_lock embedded_kernel_lock{
			embedded_kernel_run_mutex, std::defer_lock};
		if (native_kernel_entry_ == nullptr)
			embedded_kernel_lock.lock();
#endif
		::uml::runtime::RunScope run_scope{runtime_};
		if (!run_scope.entered())
			return -UML_EMULATOR_RUNTIME_ERROR;
		::uml::runtime::ActiveContextScope context_scope{runtime_};
		if (runtime_.set_execution_engine(prepared.engine) != 0)
			return -UML_EMULATOR_UNSUPPORTED;
		auto &arguments = prepared.arguments;
		std::vector<char *> argument_pointers;
		argument_pointers.reserve(arguments.size() + 1);
		for (auto &argument : arguments)
			argument_pointers.push_back(argument.data());
		argument_pointers.push_back(nullptr);

		std::vector<std::string> environment_strings;
		if (envp != nullptr) {
			for (auto entry = envp; *entry != nullptr; ++entry)
				environment_strings.emplace_back(*entry);
		}
		std::vector<char *> environment_pointers;
		environment_pointers.reserve(environment_strings.size() + 1);
		for (auto &entry : environment_strings)
			environment_pointers.push_back(entry.data());
		environment_pointers.push_back(nullptr);
		NetworkSession network_session{network_context_, network_};

		if (native_kernel_entry_ != nullptr)
			return native_kernel_entry_(
				static_cast<int>(arguments.size()),
				argument_pointers.data(), environment_pointers.data());
#if UML_EMULATOR_HAS_KERNEL_RUNNER
		return uml_elf_run(kernel_.data(), kernel_.size(),
			static_cast<int>(arguments.size()), argument_pointers.data(),
			environment_pointers.data());
#else
		return -UML_EMULATOR_UNSUPPORTED;
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
		runtime_.request_stop();
	}

	[[nodiscard]] darwin_uml_execution_engine execution_engine() const noexcept
	{
		return runtime_.execution_engine();
	}

private:
	class NetworkSession final {
	public:
		NetworkSession(uml::network::ProviderContext &context,
			uml::network::Provider provider)
			: provider_{provider}, scope_{context}
		{
		}

		~NetworkSession()
		{
			if (provider_.stop != nullptr) {
				try {
					provider_.stop(provider_.context);
				} catch (...) {
					/* A provider must not unwind through public C cleanup. */
				}
			}
		}

		NetworkSession(const NetworkSession &) = delete;
		NetworkSession &operator=(const NetworkSession &) = delete;

	private:
		uml::network::Provider provider_;
		uml::network::ActiveProviderScope scope_;
	};

	[[nodiscard]] static uml::network::Provider network_provider(
		const uml_emulator_config &config, void *default_context) noexcept
	{
		if (config.network != nullptr)
			return {
				.context = config.network->context,
				.open = config.network->open,
				.irq_fd = config.network->irq_fd,
				.read = config.network->read,
				.write = config.network->write,
			};
#if UML_EMULATOR_HAS_SLIRP
		return {
			.context = default_context,
			.open = uml_slirp_net_open,
			.irq_fd = uml_slirp_net_irq_fd,
			.read = uml_slirp_net_read,
			.write = uml_slirp_net_write,
			.stop = uml_slirp_net_stop,
		};
#else
		(void)default_context;
		return {};
#endif
	}

	[[nodiscard]] static bool uses_default_user_network(
		const uml_emulator_config &config) noexcept
	{
#if UML_EMULATOR_HAS_SLIRP
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

	std::string rootfs_;
	std::vector<std::byte> kernel_;
	uml_emulator_kernel_entry native_kernel_entry_{};
	::uml::runtime::Context runtime_;
	DefaultNetwork default_network_;
	uml::network::Provider network_;
	uml::network::ProviderContext network_context_;
	bool default_user_network_{};
};

} // namespace uml::emulator

struct uml_emulator {
	explicit uml_emulator(const uml_emulator_config &config) : implementation(config) {}

	uml::emulator::Emulator implementation;
};

extern "C" int uml_emulator_create(const uml_emulator_config *config,
				  uml_emulator **result)
{
	if (config == nullptr || result == nullptr ||
	    config->abi_version != UML_EMULATOR_ABI_VERSION ||
	    config->struct_size < sizeof(*config) ||
	    (config->network != nullptr &&
	     (config->network->struct_size < sizeof(*config->network) ||
	      config->network->open == nullptr ||
	      config->network->irq_fd == nullptr ||
	      config->network->read == nullptr ||
	      config->network->write == nullptr)))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	*result = nullptr;
	std::unique_ptr<uml_emulator> emulator;
	try {
		emulator.reset(new uml_emulator(*config));
	} catch (const std::bad_alloc &) {
		return -UML_EMULATOR_OUT_OF_MEMORY;
	} catch (...) {
		return -UML_EMULATOR_RUNTIME_ERROR;
	}
	*result = emulator.release();
	return UML_EMULATOR_OK;
}

extern "C" void uml_emulator_destroy(uml_emulator *emulator)
{
	std::unique_ptr<uml_emulator>{emulator};
}

extern "C" int uml_emulator_get_capabilities(
	const uml_emulator *emulator, uml_emulator_capabilities *capabilities)
{
	if (emulator == nullptr || capabilities == nullptr ||
	    capabilities->struct_size < sizeof(*capabilities))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	capabilities->abi_version = UML_EMULATOR_ABI_VERSION;
	capabilities->userspace_execution =
		static_cast<uml_emulator_execution_model>(
			emulator->implementation.execution_engine());
	const bool native_kernel = emulator->implementation.has_native_kernel();
	const bool elf_kernel = UML_EMULATOR_HAS_KERNEL_RUNNER &&
		emulator->implementation.has_embedded_kernel();
	capabilities->can_run = native_kernel || elf_kernel;
	capabilities->kernel_is_native_macho = native_kernel;
	capabilities->sandbox_safe = native_kernel || !elf_kernel;
	capabilities->platform = UML_APPLE_PLATFORM_NAME;
	return UML_EMULATOR_OK;
}

extern "C" int uml_emulator_run(uml_emulator *emulator, int argc,
			       const char *const argv[], const char *const envp[])
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	try {
		return emulator->implementation.run(argc, argv, envp);
	} catch (const std::bad_alloc &) {
		return -UML_EMULATOR_OUT_OF_MEMORY;
	} catch (...) {
		return -UML_EMULATOR_RUNTIME_ERROR;
	}
}

extern "C" void uml_emulator_request_stop(uml_emulator *emulator)
{
	if (emulator != nullptr)
		emulator->implementation.request_stop();
}
