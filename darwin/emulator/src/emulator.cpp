// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/emulator.h"
#include "device.hpp"
#include "options.hpp"
#include "profile_reporter.hpp"
#include "context.hpp"
#include "guest_sync.hpp"
#include "guest_console.hpp"
#include "native_kernel_layout.hpp"
#include "testing.hpp"
#include "uml_darwin_abi.h"

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
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

/* The guest reports that its writeback started, not that it finished. This is
 * the bound the host waits before flushing what the guest handed it: long
 * enough for an ordinary writeback to reach the block layer, short enough to
 * fit inside the few seconds an application has when it is being put away. */
constexpr unsigned int guest_writeback_settle_ms = 1500U;

/* Block I/O already handed to the host keeps completing after the guest stops
 * issuing more, so a frozen guest still needs a moment before its disk has
 * genuinely settled. */
constexpr unsigned int guest_suspend_settle_ms = 250U;

std::mutex process_address_space_mutex;
std::condition_variable process_address_space_changed;
std::size_t process_address_space_shared_runs;
std::size_t process_address_space_waiting_exclusive_runs;
bool process_address_space_exclusive_run;

class TranslationCacheRunScope final {
public:
	TranslationCacheRunScope() noexcept = default;

	[[nodiscard]] bool enter(bool required) noexcept
	{
		required_ = required;
		entered_ = !required ||
			::uml::runtime::acquire_translation_cache_run_session();
		return entered_;
	}

	~TranslationCacheRunScope()
	{
		if (required_ && entered_)
			::uml::runtime::release_translation_cache_run_session();
	}

	TranslationCacheRunScope(const TranslationCacheRunScope &) = delete;
	TranslationCacheRunScope &operator=(
		const TranslationCacheRunScope &) = delete;

	[[nodiscard]] bool entered() const noexcept { return entered_; }

private:
	bool required_{};
	bool entered_{};
};

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
		  network_context_{network_}
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
		/* Declared after RunScope so the session-owned executor pool drains
		 * first on scope exit. Thread states retain only lightweight JIT
		 * wrappers; Context::end_run() then retires those wrappers and their
		 * memory domain after no pooled cache can still refer to it.
		 */
		TranslationCacheRunScope translation_cache_run;
		if (!run_scope.entered())
			return -UML_EMULATOR_RUNTIME_ERROR;
		/* A process runs one guest. Refuse a later run before it names the
		 * console, maps a kernel window or reaches a kernel that would find no
		 * guest RAM left to claim. */
		if (::uml::runtime::process_guest_spent())
			return -UML_EMULATOR_UNSUPPORTED;
#if UML_EMULATOR_ADMISSION_TESTING
		if (address_space_admission_mode() !=
		    AddressSpaceAdmissionMode::none)
			invoke_admission_hook(after_handle_admission_hook);
#endif
		auto prepared = prepare_emulator_options(argc, argv, rootfs_);
		if (!prepared.valid || prepared.arguments.empty())
			return -UML_EMULATOR_INVALID_ARGUMENT;
		if (runtime_.set_execution_engine(prepared.engine) != 0)
			return -UML_EMULATOR_UNSUPPORTED;
		runtime_.set_top_byte_ignore(prepared.top_byte_ignore);
		const bool jit_run = !prepared.informational &&
			runtime_.execution_engine() ==
				::uml::runtime::ExecutionEngine::jit;
		/* Native callback handles remain independent embedding domains and use
		 * standalone executors. Only the embedded kernel activates the managed
		 * multi-vCPU executor pool.
		 */
		const bool managed_jit_run = jit_run &&
			native_kernel_entry_ == nullptr;
		/* Before any JIT engine exists, so every executor derives its cache
		 * geometry from the same budget. Data-only engines neither allocate
		 * these resources nor claim the process-global configuration.
		 */
		if (jit_run &&
		    !::uml::runtime::set_translation_cache_budget(
				prepared.jit_cache_budget, prepared.guest_cpus,
				prepared.jit_executor_cache_budget))
			return -UML_EMULATOR_RUNTIME_ERROR;
		if (!translation_cache_run.enter(managed_jit_run))
			return -UML_EMULATOR_RUNTIME_ERROR;
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
		/* Another run may have claimed the guest while this one waited for
		 * admission. */
		if (::uml::runtime::process_guest_spent())
			return -UML_EMULATOR_UNSUPPORTED;
		/* A process killed before its guest shut down left that guest's
		 * console socket behind, and this guest could not bind over it. */
		if (!prepared.informational)
			::uml::device::release_guest_management_console();
		::uml::runtime::NativeKernelLayout native_layout;
		std::optional<::uml::runtime::NativeKernelLayoutScope> layout_scope;
		if (native_kernel_entry_ == nullptr) {
			native_layout = ::uml::runtime::prepare_native_kernel_layout(
				prepared.guest_memory_bytes);
			if (!native_layout.valid)
				return native_layout.reservation_failed ?
					-UML_EMULATOR_RUNTIME_ERROR :
					-UML_EMULATOR_INVALID_ARGUMENT;
			layout_scope.emplace(native_layout);
			if (!layout_scope->entered())
				return -UML_EMULATOR_RUNTIME_ERROR;
		}
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
		const auto result = uml_elf_run(kernel_.data(), kernel_.size(),
			native_layout.load_bias,
			native_layout.reserved ? native_layout.window_start : 0U,
			native_layout.reserved ? native_layout.window_end : 0U,
			static_cast<int>(arguments.size()), argument_pointers.data(),
			environment_pointers.data());
#if UML_ENABLE_JIT_PROFILE
		/* The maintained embedded entry returns only after its execution
		 * carriers have retired. Capture the quiescent profile before the
		 * session-owned executor pool releases several GiB of cache storage;
		 * the app boundary repeats this idempotently before its std::_Exit().
		 */
		report_jit_profiles();
#endif
		return result;
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

	[[nodiscard]] int quiesce() noexcept
	{
		std::lock_guard lock{guest_lifecycle_mutex_};
		if (guest_suspension_ == ::uml::device::GuestSuspension::none) {
			guest_suspension_ = ::uml::device::suspend_guest(
				guest_writeback_settle_ms,
				guest_suspend_settle_ms);
		}
		const auto result = ::uml::runtime::sync_guest_files();
		return static_cast<int>(result.failed);
	}

	void resume() noexcept
	{
		std::lock_guard lock{guest_lifecycle_mutex_};
		const auto suspension = std::exchange(guest_suspension_,
			::uml::device::GuestSuspension::none);
		::uml::device::resume_guest(suspension);
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
	std::mutex guest_lifecycle_mutex_;
	::uml::device::GuestSuspension guest_suspension_ {
		::uml::device::GuestSuspension::none};
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

extern "C" int uml_emulator_sync(uml_emulator *emulator)
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	/* Ask the guest first. Only it can write back its own page cache, and
	 * flushing the host before it would push down a prefix of what the guest
	 * still holds. A guest without a management console simply does not
	 * answer, which leaves the host flush doing what it did before. */
	(void)::uml::device::request_guest_writeback(
		::uml::emulator::guest_writeback_settle_ms);
	const auto result = ::uml::runtime::sync_guest_files();
	return static_cast<int>(result.failed);
}

extern "C" int uml_emulator_quiesce(uml_emulator *emulator)
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	return emulator->implementation.quiesce();
}

extern "C" void uml_emulator_resume(uml_emulator *emulator)
{
	if (emulator != nullptr)
		emulator->implementation.resume();
}
