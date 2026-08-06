// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/emulator.h"
#include "device.hpp"
#include "options.hpp"
#include "context.hpp"
#include "testing.hpp"
#include "uml_darwin_abi.h"

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if UML_EMULATOR_HAS_KERNEL_RUNNER
extern "C" {
#include "elf_loader.h"
}
#endif

namespace uml::emulator {

namespace {
std::mutex process_address_space_mutex;
std::condition_variable process_address_space_changed;
std::size_t process_address_space_shared_runs;
std::size_t process_address_space_waiting_exclusive_runs;
bool process_address_space_exclusive_run;

enum class AddressSpaceAdmissionMode : std::uint8_t {
	none,
	shared,
	exclusive,
};

class PendingExclusiveAdmission final {
public:
	explicit PendingExclusiveAdmission(
		AddressSpaceAdmissionMode mode) noexcept
		: pending_{mode == AddressSpaceAdmissionMode::exclusive}
	{
		if (pending_)
			++process_address_space_waiting_exclusive_runs;
	}

	~PendingExclusiveAdmission()
	{
		if (!pending_)
			return;
		retire();
		/* A shared admission may already be sleeping solely because this
		 * writer was pending. Exceptional wait unwinding must wake it after
		 * removing the writer preference. */
		process_address_space_changed.notify_all();
	}

	PendingExclusiveAdmission(const PendingExclusiveAdmission &) = delete;
	PendingExclusiveAdmission &operator=(
		const PendingExclusiveAdmission &) = delete;

	void retire() noexcept
	{
		if (!pending_)
			return;
		--process_address_space_waiting_exclusive_runs;
		pending_ = false;
	}

private:
	bool pending_{};
};

#if UML_EMULATOR_ADMISSION_TESTING
std::atomic<testing::AddressSpaceAdmissionHook> after_handle_admission_hook;
std::atomic<testing::AddressSpaceAdmissionHook> admission_wait_boundary_hook;
std::atomic<testing::AddressSpaceAdmissionHook> before_gate_release_hook;
std::atomic<void *> address_space_admission_hook_context;
std::atomic_bool fail_next_admission_wait;

void invoke_admission_hook(
	const std::atomic<testing::AddressSpaceAdmissionHook> &hook) noexcept
{
	if (const auto callback = hook.load(std::memory_order_acquire);
	    callback != nullptr)
			callback(address_space_admission_hook_context.load(
				std::memory_order_acquire));
}

void admission_wait_checkpoint()
{
	if (fail_next_admission_wait.exchange(false, std::memory_order_acq_rel)) {
		throw std::system_error{
			std::make_error_code(std::errc::resource_unavailable_try_again),
			"forced process-address-space admission wait failure"};
	}
}
#endif

class ProcessAddressSpaceAdmission final {
public:
	explicit ProcessAddressSpaceAdmission(
		AddressSpaceAdmissionMode mode) noexcept
		: mode_{mode}
	{
	}

	[[nodiscard]] bool enter(::uml::runtime::Context &context)
	{
		if (mode_ == AddressSpaceAdmissionMode::none)
			return true;
		std::unique_lock lock{process_address_space_mutex};
		/* Declared after the lock so exceptional wait unwinding decrements the
		 * writer preference count while the admission mutex is still owned. */
		PendingExclusiveAdmission pending_exclusive{mode_};
		const auto blocked = [&] {
			return process_address_space_exclusive_run ||
				(mode_ == AddressSpaceAdmissionMode::exclusive &&
				 process_address_space_shared_runs != 0) ||
				(mode_ == AddressSpaceAdmissionMode::shared &&
				 process_address_space_waiting_exclusive_runs != 0);
		};
		while (blocked() && !context.stop_requested()) {
#if UML_EMULATOR_ADMISSION_TESTING
			invoke_admission_hook(admission_wait_boundary_hook);
			admission_wait_checkpoint();
#endif
			process_address_space_changed.wait(lock);
		}
		if (context.stop_requested()) {
			pending_exclusive.retire();
			lock.unlock();
			process_address_space_changed.notify_all();
			return false;
		}
		if (mode_ == AddressSpaceAdmissionMode::shared)
			++process_address_space_shared_runs;
		else {
			pending_exclusive.retire();
			process_address_space_exclusive_run = true;
		}
		entered_ = true;
		return true;
	}

	~ProcessAddressSpaceAdmission()
	{
		if (!entered_)
			return;
#if UML_EMULATOR_ADMISSION_TESTING
		invoke_admission_hook(before_gate_release_hook);
#endif
		{
			std::lock_guard lock{process_address_space_mutex};
			if (mode_ == AddressSpaceAdmissionMode::shared)
				--process_address_space_shared_runs;
			else
				process_address_space_exclusive_run = false;
		}
		process_address_space_changed.notify_all();
	}

	ProcessAddressSpaceAdmission(const ProcessAddressSpaceAdmission &) = delete;
	ProcessAddressSpaceAdmission &operator=(
		const ProcessAddressSpaceAdmission &) = delete;

private:
	AddressSpaceAdmissionMode mode_{AddressSpaceAdmissionMode::none};
	bool entered_{};
};
}

#if UML_EMULATOR_ADMISSION_TESTING
namespace testing {

void set_address_space_admission_hooks(
		AddressSpaceAdmissionHook after_handle_admission,
		AddressSpaceAdmissionHook at_wait_boundary,
		AddressSpaceAdmissionHook before_gate_release, void *context) noexcept
{
	address_space_admission_hook_context.store(context, std::memory_order_release);
	after_handle_admission_hook.store(
		after_handle_admission, std::memory_order_release);
	admission_wait_boundary_hook.store(
		at_wait_boundary, std::memory_order_release);
	before_gate_release_hook.store(
		before_gate_release, std::memory_order_release);
}

void fail_next_address_space_admission_wait() noexcept
{
	fail_next_admission_wait.store(true, std::memory_order_release);
}

} // namespace testing
#endif

class Emulator final {
public:
	explicit Emulator(const uml_emulator_config &config)
		: rootfs_(config.rootfs_path == nullptr ? "" : config.rootfs_path),
		  native_kernel_entry_{config.native_kernel_entry},
		  runtime_{},
		  network_{network_provider(config)},
		  network_context_{network_},
		  user_network_{config.network != nullptr}
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
		ProcessAddressSpaceAdmission process_admission{
			address_space_admission_mode()};
		::uml::runtime::RunScope run_scope{runtime_};
		if (!run_scope.entered())
			return -UML_EMULATOR_RUNTIME_ERROR;
#if UML_EMULATOR_ADMISSION_TESTING
		if (address_space_admission_mode() !=
		    AddressSpaceAdmissionMode::none)
			invoke_admission_hook(after_handle_admission_hook);
#endif
		auto prepared = prepare_emulator_options(
			argc, argv, rootfs_, user_network_);
		if (!prepared.valid || prepared.arguments.empty())
			return -UML_EMULATOR_INVALID_ARGUMENT;
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
		if (!process_admission.enter(runtime_))
			return -UML_EMULATOR_RUNTIME_ERROR;
		/* One opaque route publishes CPU and network endpoints under one lock.
		 * Its intrusive storage and commit are allocation-free. */
		::uml::runtime::ActiveRunRouteScope route_scope{
			runtime_, &network_context_,
			::uml::runtime::ActiveRunRouteScope::DeferredPublication{}};
		route_scope.commit();

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
		if (address_space_admission_mode() !=
		    AddressSpaceAdmissionMode::none) {
			/* Predicate mutation and wait enrollment share this mutex, so a
			 * notification cannot land between a false check and wait. */
			{
				std::lock_guard lock{process_address_space_mutex};
				runtime_.request_stop();
			}
			process_address_space_changed.notify_all();
			return;
		}
		runtime_.request_stop();
	}

	[[nodiscard]] ::uml::runtime::ExecutionEngine
	execution_engine() const noexcept
	{
		return runtime_.execution_engine();
	}

private:
	[[nodiscard]] AddressSpaceAdmissionMode
	address_space_admission_mode() const noexcept
	{
		if (native_kernel_entry_ != nullptr) {
#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
			return AddressSpaceAdmissionMode::exclusive;
#else
			return AddressSpaceAdmissionMode::shared;
#endif
		}
		return kernel_.empty() ? AddressSpaceAdmissionMode::none :
			AddressSpaceAdmissionMode::exclusive;
	}

	[[nodiscard]] static uml::network::Provider network_provider(
		const uml_emulator_config &config) noexcept
	{
		if (config.network != nullptr)
			return {
				.context = config.network->context,
				.open = config.network->open,
				.irq_fd = config.network->irq_fd,
				.read = config.network->read,
				.write = config.network->write,
			};
		return {};
	}

	std::string rootfs_;
	std::vector<std::byte> kernel_;
	uml_emulator_kernel_entry native_kernel_entry_{};
	::uml::runtime::Context runtime_;
	uml::network::Provider network_;
	uml::network::ProviderContext network_context_;
	bool user_network_{};
};

} // namespace uml::emulator

struct uml_emulator {
	explicit uml_emulator(const uml_emulator_config &config) : implementation(config) {}

	uml::emulator::Emulator implementation;
};

extern "C" int uml_emulator_create(const uml_emulator_config *config,
				  uml_emulator **result)
{
	if (result == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	*result = nullptr;
	if (config == nullptr ||
	    config->abi_version != UML_EMULATOR_ABI_VERSION ||
	    config->struct_size < sizeof(*config) ||
	    (config->kernel_image == nullptr && config->kernel_image_size != 0) ||
	    (config->network != nullptr &&
	     (config->network->struct_size < sizeof(*config->network) ||
	      config->network->open == nullptr ||
	      config->network->irq_fd == nullptr ||
	      config->network->read == nullptr ||
	      config->network->write == nullptr)))
		return -UML_EMULATOR_INVALID_ARGUMENT;
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
