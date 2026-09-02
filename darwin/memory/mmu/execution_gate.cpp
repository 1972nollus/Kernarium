// SPDX-License-Identifier: GPL-2.0-or-later
#include "execution_gate.hpp"

#include <atomic>
#include <utility>

#ifndef UML_RUNTIME_PROFILING
#define UML_RUNTIME_PROFILING 0
#endif

#if UML_RUNTIME_PROFILING
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>
#endif

namespace uml::memory {
namespace {

constexpr std::uint64_t execution_gate_closed = std::uint64_t{1} << 63;
constexpr std::uint64_t execution_reader_mask = ~execution_gate_closed;

#if UML_RUNTIME_PROFILING
[[nodiscard]] bool gate_profile_enabled() noexcept
{
	static const bool enabled = [] {
		const auto *value = std::getenv("UML_PROFILE_TLB_GATE");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	return enabled;
}

[[nodiscard]] std::uint64_t current_thread_id() noexcept
{
	std::uint64_t identifier = 0;
	(void)pthread_threadid_np(nullptr, &identifier);
	return identifier;
}

struct ReaderProfile final {
	std::atomic_uint64_t generation{};
	std::atomic_uint64_t guest_pc{};
	std::uint64_t thread{};
};

std::mutex &reader_profiles_mutex()
{
	static auto *mutex = new std::mutex;
	return *mutex;
}

std::vector<ReaderProfile *> &reader_profiles()
{
	static auto *profiles = new std::vector<ReaderProfile *>;
	return *profiles;
}

class ReaderProfileRegistration final {
public:
	ReaderProfileRegistration() noexcept
	{
		profile_.thread = current_thread_id();
		try {
			std::lock_guard lock{reader_profiles_mutex()};
			reader_profiles().push_back(&profile_);
			registered_ = true;
		} catch (...) {
		}
	}

	~ReaderProfileRegistration()
	{
		if (!registered_)
			return;
		std::lock_guard lock{reader_profiles_mutex()};
		std::erase(reader_profiles(), &profile_);
	}

	[[nodiscard]] ReaderProfile *get() noexcept
	{
		return registered_ ? &profile_ : nullptr;
	}

private:
	ReaderProfile profile_{};
	bool registered_{};
};

ReaderProfile *local_reader_profile() noexcept
{
	static thread_local ReaderProfileRegistration registration;
	return registration.get();
}

void profile_reader_begin(
	std::uint64_t generation, std::uint64_t guest_pc) noexcept
{
	if (!gate_profile_enabled())
		return;
	if (auto *profile = local_reader_profile(); profile != nullptr) {
		profile->guest_pc.store(guest_pc, std::memory_order_relaxed);
		profile->generation.store(generation, std::memory_order_release);
	}
}

void profile_reader_end() noexcept
{
	if (!gate_profile_enabled())
		return;
	if (auto *profile = local_reader_profile(); profile != nullptr) {
		profile->generation.store(0, std::memory_order_release);
		profile->guest_pc.store(0, std::memory_order_relaxed);
	}
}

void profile_stalled_readers(std::uint64_t generation)
{
	const auto *path = std::getenv("UML_PROFILE_TLB_GATE");
	std::FILE *output = stderr;
	if (path != nullptr && path[0] == '/') {
		if (auto *file = std::fopen(path, "a"); file != nullptr)
			output = file;
	}
	std::fprintf(output,
		"Darwin static MMU: writer thread=%llu generation=%llu waiting for execution reader(s):",
		static_cast<unsigned long long>(current_thread_id()),
		static_cast<unsigned long long>(generation));
	{
		std::lock_guard lock{reader_profiles_mutex()};
		for (const auto *profile : reader_profiles()) {
			if (profile->generation.load(std::memory_order_acquire) !=
			    generation)
				continue;
			std::fprintf(output, " thread=%llu guest-pc=%#llx",
				static_cast<unsigned long long>(profile->thread),
				static_cast<unsigned long long>(profile->guest_pc.load(
					std::memory_order_acquire)));
		}
	}
	std::fputc('\n', output);
	std::fflush(output);
	if (output != stderr)
		std::fclose(output);
}
#endif

} // namespace

bool execution_reader_begin(GenerationState &state, std::uint64_t generation,
	std::uint64_t guest_pc) noexcept
{
	auto value = state.execution_gate.load(std::memory_order_acquire);
	for (;;) {
		if (state.retired.load(std::memory_order_acquire))
			return false;
		while ((value & execution_gate_closed) != 0) {
			state.execution_gate.wait(value, std::memory_order_relaxed);
			value = state.execution_gate.load(std::memory_order_acquire);
			if (state.retired.load(std::memory_order_acquire))
				return false;
		}
		if ((value & execution_reader_mask) == execution_reader_mask)
			return false;
		if (state.execution_gate.compare_exchange_weak(
				value, value + 1, std::memory_order_acquire,
				std::memory_order_relaxed)) {
#if UML_RUNTIME_PROFILING
			profile_reader_begin(generation, guest_pc);
#else
			(void)generation;
			(void)guest_pc;
#endif
			return true;
		}
	}
}

void execution_reader_end(GenerationState &state) noexcept
{
#if UML_RUNTIME_PROFILING
	profile_reader_end();
#endif
	const auto previous = state.execution_gate.fetch_sub(
		1, std::memory_order_release);
	if ((previous & execution_gate_closed) != 0 &&
	    (previous & execution_reader_mask) == 1)
		state.execution_gate.notify_all();
}

bool execution_context_publish_code_change(void *opaque) noexcept
{
	auto *context = static_cast<ExecutionContext *>(opaque);
	if (context == nullptr || !context->entered || !context->state ||
	    context->state->retired.load(std::memory_order_acquire))
		return false;
	/* The entered execution context owns the generation state for the entire
	 * translated run, so publication needs no domain lookup or mapping lock. */
	context->state->code_changes.fetch_add(1, std::memory_order_release);
	return true;
}

ExecutionQuiesce::ExecutionQuiesce(std::shared_ptr<GenerationState> state,
	std::uint64_t generation)
	: state_{std::move(state)}
#if UML_RUNTIME_PROFILING
	, generation_{generation}
#endif
{
#if !UML_RUNTIME_PROFILING
	(void)generation;
#endif
	auto value = state_->execution_gate.load(std::memory_order_acquire);
	for (;;) {
		if ((value & execution_gate_closed) != 0) {
			state_->execution_gate.wait(value, std::memory_order_relaxed);
			value = state_->execution_gate.load(std::memory_order_acquire);
			continue;
		}
		if (state_->execution_gate.compare_exchange_weak(
				value, value | execution_gate_closed,
				std::memory_order_acq_rel,
				std::memory_order_relaxed))
			break;
	}
	state_->execution_interrupts.fetch_add(1, std::memory_order_release);
	value = state_->execution_gate.load(std::memory_order_acquire);
#if UML_RUNTIME_PROFILING
	const auto started = std::chrono::steady_clock::now();
	bool reported = false;
	while ((value & execution_reader_mask) != 0) {
		if (!gate_profile_enabled()) {
			state_->execution_gate.wait(value, std::memory_order_relaxed);
		} else {
			if (!reported && std::chrono::steady_clock::now() - started >=
				    std::chrono::milliseconds{100}) {
				profile_stalled_readers(generation_);
				reported = true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds{1});
		}
		value = state_->execution_gate.load(std::memory_order_acquire);
	}
#else
	while ((value & execution_reader_mask) != 0) {
		state_->execution_gate.wait(value, std::memory_order_relaxed);
		value = state_->execution_gate.load(std::memory_order_acquire);
	}
#endif
}

ExecutionQuiesce::~ExecutionQuiesce()
{
	state_->execution_gate.store(0, std::memory_order_release);
	state_->execution_gate.notify_all();
}

#if UML_RUNTIME_PROFILING && UML_MEMORY_PROFILE_TESTING
namespace testing {

std::size_t execution_gate_profile_count() noexcept
{
	std::lock_guard lock{reader_profiles_mutex()};
	return reader_profiles().size();
}

} // namespace testing
#endif

} // namespace uml::memory
