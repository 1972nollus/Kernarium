// SPDX-License-Identifier: GPL-2.0-or-later
#include "execution_gate.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>

namespace uml::memory {
namespace {

constexpr std::uint64_t execution_gate_closed = std::uint64_t{1} << 63;
constexpr std::uint64_t execution_reader_mask = ~execution_gate_closed;

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
	static std::mutex mutex;
	return mutex;
}

std::vector<std::unique_ptr<ReaderProfile>> &reader_profiles()
{
	static std::vector<std::unique_ptr<ReaderProfile>> profiles;
	return profiles;
}

ReaderProfile &local_reader_profile()
{
	static thread_local ReaderProfile *profile = [] {
		auto owned = std::make_unique<ReaderProfile>();
		owned->thread = current_thread_id();
		auto *result = owned.get();
		std::lock_guard lock{reader_profiles_mutex()};
		reader_profiles().push_back(std::move(owned));
		return result;
	}();
	return *profile;
}

void profile_reader_begin(std::uint64_t generation, std::uint64_t guest_pc)
{
	if (!gate_profile_enabled())
		return;
	auto &profile = local_reader_profile();
	profile.guest_pc.store(guest_pc, std::memory_order_relaxed);
	profile.generation.store(generation, std::memory_order_release);
}

void profile_reader_end()
{
	if (!gate_profile_enabled())
		return;
	auto &profile = local_reader_profile();
	profile.generation.store(0, std::memory_order_release);
	profile.guest_pc.store(0, std::memory_order_relaxed);
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
		for (const auto &profile : reader_profiles()) {
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

} // namespace

bool execution_reader_begin(GenerationState &state, std::uint64_t generation,
	std::uint64_t guest_pc)
{
	auto value = state.execution_gate.load(std::memory_order_acquire);
	for (;;) {
		while ((value & execution_gate_closed) != 0) {
			state.execution_gate.wait(value, std::memory_order_relaxed);
			value = state.execution_gate.load(std::memory_order_acquire);
		}
		if ((value & execution_reader_mask) == execution_reader_mask)
			return false;
		if (state.execution_gate.compare_exchange_weak(
				value, value + 1, std::memory_order_acquire,
				std::memory_order_relaxed)) {
			profile_reader_begin(generation, guest_pc);
			return true;
		}
	}
}

void execution_reader_end(GenerationState &state)
{
	profile_reader_end();
	const auto previous = state.execution_gate.fetch_sub(
		1, std::memory_order_release);
	if ((previous & execution_gate_closed) != 0 &&
	    (previous & execution_reader_mask) == 1)
		state.execution_gate.notify_all();
}

ExecutionQuiesce::ExecutionQuiesce(std::shared_ptr<GenerationState> state,
	std::uint64_t generation)
	: state_{std::move(state)}, generation_{generation}
{
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
}

ExecutionQuiesce::~ExecutionQuiesce()
{
	state_->execution_gate.store(0, std::memory_order_release);
	state_->execution_gate.notify_all();
}

} // namespace uml::memory
