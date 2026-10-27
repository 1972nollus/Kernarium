// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/emulator.h"
#include "device.hpp"
#include "options.hpp"
#include "context.hpp"
#include "guest_sync.hpp"
#include "guest_console.hpp"
#include "guest_hibernation.hpp"
#include "kernel_signal_scope.hpp"
#include "native_kernel_layout.hpp"
#include "testing.hpp"
#include "uml_darwin_abi.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if UML_EMULATOR_HAS_KERNEL_RUNNER
extern "C" {
#include "elf_loader.h"
}
#endif

#if UML_EMULATOR_TESTING
#include <cstring>
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

/* A guest about to be suspended hands its cold page cache and free memory to
 * the host first, since it keeps whatever it holds until it runs again. The
 * guest reclaims for at most a second and compacts for at most another, so
 * this also covers the release and the reply. A pass still running when it
 * expires finishes before the guest takes the suspend request queued behind
 * it. */
constexpr unsigned int guest_trim_wait_ms = 3000U;

[[nodiscard]] int guest_request_result(
	::uml::device::GuestRequest request) noexcept
{
	switch (request) {
	case ::uml::device::GuestRequest::done:
		return UML_EMULATOR_OK;
	case ::uml::device::GuestRequest::unanswered:
		return -UML_EMULATOR_RUNTIME_ERROR;
	case ::uml::device::GuestRequest::unreachable:
	case ::uml::device::GuestRequest::refused:
		break;
	}
	return -UML_EMULATOR_UNSUPPORTED;
}

/* An embedded kernel maps fixed host addresses, so a process admits one run
 * at a time. A run queued behind it stays cancellable by its handle's stop
 * request. */
std::mutex process_address_space_mutex;
std::condition_variable process_address_space_changed;
bool process_address_space_admitted;

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

#if UML_EMULATOR_TESTING
constexpr char test_kernel_magic[] = "uml-test-kernel";

[[nodiscard]] testing::KernelEntry test_kernel_entry(
	std::span<const std::byte> image) noexcept
{
	testing::KernelEntry entry{};
	if (image.size() != sizeof(test_kernel_magic) + sizeof(entry) ||
	    std::memcmp(image.data(), test_kernel_magic,
		    sizeof(test_kernel_magic)) != 0)
		return nullptr;
	std::memcpy(&entry, image.data() + sizeof(test_kernel_magic),
		sizeof(entry));
	return entry;
}

std::atomic<testing::AddressSpaceAdmissionHook> after_handle_admission_hook;
std::atomic<testing::AddressSpaceAdmissionHook> admission_wait_boundary_hook;
std::atomic<testing::AddressSpaceAdmissionHook> before_gate_release_hook;
std::atomic<void *> address_space_admission_hook_context;

void invoke_admission_hook(
	const std::atomic<testing::AddressSpaceAdmissionHook> &hook) noexcept
{
	if (const auto callback = hook.load(std::memory_order_acquire);
	    callback != nullptr)
			callback(address_space_admission_hook_context.load(
				std::memory_order_acquire));
}

/* A Darwin sanitizer holds the product's macOS window itself (AddressSanitizer
 * keeps its heap at 0x600000000000, ThreadSanitizer reserves everything
 * outside its application ranges), and the process reserves its window for
 * good. A sanitized test variant therefore places runs at the start of
 * ThreadSanitizer's middle application range, which AddressSanitizer leaves to
 * the application too, unless a test places them elsewhere. */
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define UML_EMULATOR_TEST_SANITIZED 1
#endif
#endif
#ifdef UML_EMULATOR_TEST_SANITIZED
constexpr std::uintptr_t default_test_native_window_start = 0x550000000000ULL;
#else
constexpr std::uintptr_t default_test_native_window_start = 0;
#endif
std::atomic<std::uintptr_t> test_native_kernel_window_start{
	default_test_native_window_start};
#endif

/* Where a run's native kernel window goes: the product's placement, or in the
 * test variant the same geometry wherever a test placed the window. The
 * process holds its window for as long as it lives, so a run takes the window
 * it holds or extends it. */
[[nodiscard]] ::uml::runtime::NativeKernelLayout run_native_kernel_layout(
	std::size_t guest_memory) noexcept
{
#if UML_EMULATOR_TESTING
	const auto start = test_native_kernel_window_start.load(
		std::memory_order_acquire);
	auto product = ::uml::runtime::native_kernel_placement(guest_memory);
	if (start != 0 && product.geometry.valid &&
	    product.placement == ::uml::runtime::WindowPlacement::exact) {
		auto &geometry = product.geometry;
		const auto length = geometry.window_end - geometry.window_start;
		geometry.load_bias += start - geometry.window_start;
		geometry.window_start = start;
		geometry.window_end = start + length;
		const auto layout = ::uml::runtime::reserve_process_native_window(
			geometry, product.placement);
		return layout.valid ? layout :
			::uml::runtime::NativeKernelLayout{.reservation_failed = true};
	}
#endif
	return ::uml::runtime::prepare_native_kernel_layout(guest_memory);
}

class ProcessAddressSpaceAdmission final {
public:
	ProcessAddressSpaceAdmission() noexcept = default;

	[[nodiscard]] bool enter(::uml::runtime::Context &context)
	{
		std::unique_lock lock{process_address_space_mutex};
		while (process_address_space_admitted &&
		       !context.stop_requested()) {
#if UML_EMULATOR_TESTING
			invoke_admission_hook(admission_wait_boundary_hook);
#endif
			process_address_space_changed.wait(lock);
		}
		if (context.stop_requested())
			return false;
		process_address_space_admitted = true;
		entered_ = true;
		return true;
	}

	~ProcessAddressSpaceAdmission()
	{
		if (!entered_)
			return;
#if UML_EMULATOR_TESTING
		invoke_admission_hook(before_gate_release_hook);
#endif
		{
			std::lock_guard lock{process_address_space_mutex};
			process_address_space_admitted = false;
		}
		process_address_space_changed.notify_all();
	}

	ProcessAddressSpaceAdmission(const ProcessAddressSpaceAdmission &) = delete;
	ProcessAddressSpaceAdmission &operator=(
		const ProcessAddressSpaceAdmission &) = delete;

private:
	bool entered_{};
};
}

#if UML_EMULATOR_TESTING
namespace testing {

std::vector<std::byte> kernel_image(KernelEntry entry)
{
	std::vector<std::byte> image(sizeof(test_kernel_magic) + sizeof(entry));
	std::memcpy(image.data(), test_kernel_magic, sizeof(test_kernel_magic));
	std::memcpy(image.data() + sizeof(test_kernel_magic), &entry,
		sizeof(entry));
	return image;
}

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

void place_native_kernel_window(std::uintptr_t start) noexcept
{
	test_native_kernel_window_start.store(start, std::memory_order_release);
}

void report_guest_shutdown(::uml::runtime::GuestShutdownKind kind,
	const char *command) noexcept
{
	if (auto *const context = ::uml::runtime::current_context();
	    context != nullptr)
		context->record_guest_shutdown(kind,
			command == nullptr ? std::string_view{} :
				std::string_view{command});
}

NativeKernelWindow native_kernel_window(std::size_t guest_memory) noexcept
{
	const auto layout = run_native_kernel_layout(guest_memory);
	if (!layout.valid)
		return {};
	return {.start = layout.window_start, .end = layout.window_end};
}

} // namespace testing
#endif

class Emulator final {
public:
	explicit Emulator(const uml_emulator_config &config)
		: rootfs_(config.rootfs_path == nullptr ? "" : config.rootfs_path),
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
		if (kernel_.empty())
			return -UML_EMULATOR_UNSUPPORTED;
		ProcessAddressSpaceAdmission process_admission;
		::uml::runtime::RunScope run_scope{runtime_};
		/* Declared after RunScope so the session-owned executor pool drains
		 * first on scope exit. Thread states retain only lightweight JIT
		 * wrappers; Context::end_run() then retires those wrappers and their
		 * memory domain after no pooled cache can still refer to it.
		 */
		TranslationCacheRunScope translation_cache_run;
		if (!run_scope.entered())
			return -UML_EMULATOR_RUNTIME_ERROR;
		clear_run_end();
		/* A process runs one guest. Refuse a later run before it names the
		 * console, maps a kernel window or reaches a kernel that would find no
		 * guest RAM left to claim. */
		if (::uml::runtime::process_guest_spent())
			return -UML_EMULATOR_UNSUPPORTED;
#if UML_EMULATOR_TESTING
		invoke_admission_hook(after_handle_admission_hook);
#endif
		auto prepared = prepare_emulator_options(argc, argv, rootfs_);
		if (!prepared.valid || prepared.arguments.empty())
			return -UML_EMULATOR_INVALID_ARGUMENT;
		if (runtime_.set_execution_engine(prepared.engine) != 0)
			return -UML_EMULATOR_UNSUPPORTED;
		runtime_.set_top_byte_ignore(prepared.top_byte_ignore);
		if (!prepared.informational)
			prepare_guest_hibernation(prepared.guest_hibernation);
		runtime_.set_x86_jit_tier(prepared.jit_tier_instructions);
		const bool jit_run = !prepared.informational &&
			runtime_.execution_engine() ==
				::uml::runtime::ExecutionEngine::jit;
		/* Before any JIT engine exists, so every executor derives its cache
		 * geometry from the same budget. Data-only engines neither allocate
		 * these resources nor claim the process-global configuration.
		 */
		if (jit_run &&
		    !::uml::runtime::set_translation_cache_budget(
				prepared.jit_cache_budget, prepared.guest_cpus,
				prepared.jit_executor_cache_budget))
			return -UML_EMULATOR_RUNTIME_ERROR;
		/* Before any gadget executor exists, for the same reason; an
		 * x86-64 address space starts on the gadget engine in a JIT run
		 * too. */
		if (!prepared.informational)
			(void)::uml::runtime::set_gadget_cache_budget(
				prepared.guest_memory_bytes,
				prepared.gadget_cache_budget);
		/* A JIT run activates the managed multi-vCPU executor pool. */
		if (!translation_cache_run.enter(jit_run))
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
		/* Readies the guest console's location before the guest binds it,
		 * and removes it when the run returns, however the guest ended. */
		const ::uml::device::GuestConsoleRun console_run{
			!prepared.informational && prepared.host_console_directory};
		const auto native_layout = run_native_kernel_layout(
			prepared.guest_memory_bytes);
		if (!native_layout.valid)
			return native_layout.reservation_failed ?
				-UML_EMULATOR_RUNTIME_ERROR :
				-UML_EMULATOR_INVALID_ARGUMENT;
		const ::uml::runtime::NativeKernelLayoutScope layout_scope{
			native_layout};
		if (!layout_scope.entered())
			return -UML_EMULATOR_RUNTIME_ERROR;
		/* One opaque route publishes CPU and network endpoints under one lock.
		 * Its intrusive storage and commit are allocation-free. */
		::uml::runtime::ActiveRunRouteScope route_scope{
			runtime_, &network_context_,
			::uml::runtime::ActiveRunRouteScope::DeferredPublication{}};
		route_scope.commit();

		/* An informational run prints and returns: it ran no guest, so it
		 * has no end to record. */
		std::optional<RunEndRecorder> run_end_recorder;
#if UML_EMULATOR_TESTING
		if (const auto entry = test_kernel_entry(kernel_); entry != nullptr) {
			if (!prepared.informational)
				run_end_recorder.emplace(*this);
			/* The kernel's own process state is the run's to restore, as
			 * the loader restores it around a real kernel. */
			const ::uml::runtime::KernelSignalScope kernel_signals{0, 0};
			return entry(static_cast<int>(arguments.size()),
				argument_pointers.data(), environment_pointers.data());
		}
#endif
#if UML_EMULATOR_HAS_KERNEL_RUNNER
		if (!prepared.informational)
			run_end_recorder.emplace(*this);
		const auto result = uml_elf_run(kernel_.data(), kernel_.size(),
			native_layout.load_bias,
			native_layout.reserved ? native_layout.window_start : 0U,
			native_layout.reserved ? native_layout.window_end : 0U,
			static_cast<int>(arguments.size()), argument_pointers.data(),
			environment_pointers.data());
		return result;
#else
		return -UML_EMULATOR_UNSUPPORTED;
#endif
	}

	[[nodiscard]] bool has_embedded_kernel() const noexcept
	{
		return !kernel_.empty();
	}

	void request_stop() noexcept
	{
		/* A stop reaches threads that run. */
		bring_back_guest_ram();
		/* Predicate mutation and wait enrollment share this mutex, so a
		 * notification cannot land between a false check and wait. */
		{
			std::lock_guard lock{process_address_space_mutex};
			runtime_.request_stop();
		}
		process_address_space_changed.notify_all();
	}

	[[nodiscard]] int quiesce() noexcept
	{
		std::lock_guard lock{guest_lifecycle_mutex_};
		if (guest_suspension_ == ::uml::device::GuestSuspension::none) {
			(void)::uml::device::trim_guest_page_cache(
				guest_trim_wait_ms);
			guest_suspension_ = ::uml::device::suspend_guest(
				guest_writeback_settle_ms,
				guest_suspend_settle_ms);
		}
		const auto result = ::uml::runtime::sync_guest_files();
		/* Last, with the guest asleep and its files on disk. */
		if (guest_suspension_ == ::uml::device::GuestSuspension::system)
			hibernate_guest_ram();
		return static_cast<int>(result.failed);
	}

	void resume() noexcept
	{
		/* A hibernation still writing leaves what it has not reached in the
		 * process. */
		request_hibernation_abort();
		std::lock_guard lock{guest_lifecycle_mutex_};
		/* The RAM and the threads are back before the guest is woken. A
		 * restore that fails ends the process and does not return; where
		 * the end is replaced, nothing is woken either. */
		if (hibernation_ != nullptr && !hibernation_->restore())
			return;
		const auto suspension = std::exchange(guest_suspension_,
			::uml::device::GuestSuspension::none);
		::uml::device::resume_guest(suspension);
		if (const auto enabled = std::exchange(pending_page_cache_trim_,
			    std::nullopt))
			(void)::uml::device::set_guest_page_cache_trim(*enabled);
	}

	[[nodiscard]] int set_page_cache_trimming(bool enabled) noexcept
	{
		std::lock_guard lock{guest_lifecycle_mutex_};
		/* Any console request ends a suspend, so a quiesced guest
		 * receives the change when it is resumed. */
		if (guest_suspension_ != ::uml::device::GuestSuspension::none) {
			pending_page_cache_trim_ = enabled;
			return UML_EMULATOR_OK;
		}
		pending_page_cache_trim_.reset();
		return guest_request_result(
			::uml::device::set_guest_page_cache_trim(enabled));
	}

	[[nodiscard]] int trim_page_cache() noexcept
	{
		std::lock_guard lock{guest_lifecycle_mutex_};
		/* Quiescing already trimmed it, and a request would wake it. */
		if (guest_suspension_ != ::uml::device::GuestSuspension::none)
			return UML_EMULATOR_OK;
		return guest_request_result(
			::uml::device::trim_guest_page_cache(guest_trim_wait_ms));
	}

	void wake() noexcept
	{
		std::lock_guard lock{guest_lifecycle_mutex_};
		/* A quiesced guest stays put until the host resumes it. */
		if (guest_suspension_ == ::uml::device::GuestSuspension::none)
			(void)::uml::device::wake_guest();
	}

	[[nodiscard]] ::uml::runtime::ExecutionEngine
	execution_engine() const noexcept
	{
		return runtime_.execution_engine();
	}

	[[nodiscard]] uml_emulator_run_end run_end() const noexcept
	{
		std::lock_guard lock{run_end_mutex_};
		return run_end_;
	}

#if UML_EMULATOR_TESTING
	void use_hibernation(
		std::unique_ptr<::uml::runtime::HibernationSteps> steps,
		::uml::runtime::GuestHibernation::FailureHandler on_failure)
	{
		std::lock_guard lock{guest_lifecycle_mutex_};
		hibernation_ = std::make_unique<::uml::runtime::GuestHibernation>(
			std::move(steps), on_failure);
		hibernation_pointer_.store(hibernation_.get(),
			std::memory_order_release);
	}
#endif

private:
	/* A run that begins on the handle starts from no end. */
	void clear_run_end() noexcept
	{
		std::lock_guard lock{run_end_mutex_};
		run_end_ = {
			.struct_size = sizeof(run_end_),
			.kind = UML_EMULATOR_RUN_END_NONE,
			.restart_command = {},
		};
	}

	/* Says how the run's guest ended, once its kernel has returned. The host's
	 * own stop request wins over what the guest said on its way out, and a
	 * kernel that said nothing failed. */
	void record_run_end() noexcept
	{
		uml_emulator_run_end end{
			.struct_size = sizeof(end),
			.kind = UML_EMULATOR_RUN_END_FAILED,
			.restart_command = {},
		};
		const auto shutdown = runtime_.guest_shutdown();
		if (runtime_.stop_requested()) {
			end.kind = UML_EMULATOR_RUN_END_STOPPED;
		} else if (shutdown.kind ==
			   ::uml::runtime::GuestShutdownKind::restart) {
			end.kind = UML_EMULATOR_RUN_END_RESTART;
			static_assert(sizeof(end.restart_command) >
				      ::uml::runtime::guest_shutdown_command_limit);
			std::copy_n(shutdown.command,
				    ::uml::runtime::guest_shutdown_command_limit,
				    end.restart_command);
		} else if (shutdown.kind !=
			   ::uml::runtime::GuestShutdownKind::none) {
			end.kind = UML_EMULATOR_RUN_END_POWER_OFF;
		}
		std::lock_guard lock{run_end_mutex_};
		run_end_ = end;
	}

	/* Records the end of the run that launched a kernel, on every way out of
	 * the run, an exception included. */
	class RunEndRecorder final {
	public:
		explicit RunEndRecorder(Emulator &emulator) noexcept
			: emulator_{emulator}
		{
		}
		~RunEndRecorder()
		{
			emulator_.record_run_end();
		}
		RunEndRecorder(const RunEndRecorder &) = delete;
		RunEndRecorder &operator=(const RunEndRecorder &) = delete;

	private:
		Emulator &emulator_;
	};

	/* Before the guest boots: what the options say about hibernating, and
	 * the images a process that ended in a hibernation left behind. */
	void prepare_guest_hibernation(::uml::runtime::HibernationMode mode)
	{
		std::lock_guard lock{guest_lifecycle_mutex_};
		hibernation_mode_ = mode;
		if (mode != ::uml::runtime::HibernationMode::off)
			(void)::uml::runtime::remove_stale_hibernation_images(
				::uml::runtime::hibernation_directory());
	}

	void hibernate_guest_ram() noexcept
	{
		if (hibernation_ == nullptr) {
			if (hibernation_mode_ == ::uml::runtime::HibernationMode::off)
				return;
			try {
				hibernation_ = ::uml::runtime::GuestHibernation::create(
					hibernation_mode_,
					::uml::runtime::hibernation_directory());
			} catch (...) {
				return;
			}
			hibernation_pointer_.store(hibernation_.get(),
				std::memory_order_release);
		}
		(void)hibernation_->hibernate();
	}

	void request_hibernation_abort() noexcept
	{
		if (auto *const hibernation =
			    hibernation_pointer_.load(std::memory_order_acquire))
			hibernation->request_abort();
	}

	void bring_back_guest_ram() noexcept
	{
		request_hibernation_abort();
		std::lock_guard lock{guest_lifecycle_mutex_};
		if (hibernation_ != nullptr)
			(void)hibernation_->restore();
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
	::uml::runtime::Context runtime_;
	uml::network::Provider network_;
	uml::network::ProviderContext network_context_;
	std::mutex guest_lifecycle_mutex_;
	::uml::device::GuestSuspension guest_suspension_ {
		::uml::device::GuestSuspension::none};
	/* A trimming change requested while the guest was quiesced. */
	std::optional<bool> pending_page_cache_trim_;
	::uml::runtime::HibernationMode hibernation_mode_ {
		::uml::runtime::HibernationMode::automatic};
	/* Made by the first quiesce that hibernates. Guarded by the lifecycle
	 * mutex; the pointer is also published for the abort request, which
	 * resume sends before it waits for the mutex. */
	std::unique_ptr<::uml::runtime::GuestHibernation> hibernation_;
	std::atomic<::uml::runtime::GuestHibernation *> hibernation_pointer_ {
		nullptr};
	/* How the handle's most recent run ended, for uml_emulator_get_run_end. */
	mutable std::mutex run_end_mutex_;
	uml_emulator_run_end run_end_ {
		.struct_size = sizeof(uml_emulator_run_end),
		.kind = UML_EMULATOR_RUN_END_NONE,
		.restart_command = {},
	};
};

} // namespace uml::emulator

struct uml_emulator {
	explicit uml_emulator(const uml_emulator_config &config) : implementation(config) {}

	uml::emulator::Emulator implementation;
};

#if UML_EMULATOR_TESTING
namespace uml::emulator::testing {

void use_hibernation(uml_emulator *emulator,
	std::unique_ptr<::uml::runtime::HibernationSteps> steps,
	void (*on_failure)() noexcept)
{
	emulator->implementation.use_hibernation(std::move(steps), on_failure);
}

} // namespace uml::emulator::testing
#endif

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
	const bool elf_kernel = UML_EMULATOR_HAS_KERNEL_RUNNER &&
		emulator->implementation.has_embedded_kernel();
	capabilities->can_run = elf_kernel;
	capabilities->sandbox_safe = !elf_kernel;
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

extern "C" int uml_emulator_get_run_end(const uml_emulator *emulator,
					uml_emulator_run_end *end)
{
	if (emulator == nullptr || end == nullptr ||
	    end->struct_size < sizeof(*end))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	*end = emulator->implementation.run_end();
	return UML_EMULATOR_OK;
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

extern "C" void uml_emulator_wake(uml_emulator *emulator)
{
	if (emulator != nullptr)
		emulator->implementation.wake();
}

extern "C" int uml_emulator_set_page_cache_trimming(uml_emulator *emulator,
	int enabled)
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	return emulator->implementation.set_page_cache_trimming(enabled != 0);
}

extern "C" int uml_emulator_trim_page_cache(uml_emulator *emulator)
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	return emulator->implementation.trim_page_cache();
}
