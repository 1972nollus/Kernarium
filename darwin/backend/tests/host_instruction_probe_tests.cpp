// SPDX-License-Identifier: GPL-2.0-or-later

#include "runtime/host_features.hpp"

#include <cstdlib>
#include <csignal>

namespace {

volatile std::sig_atomic_t parent_sigill_count;

void parent_sigill_handler(int) noexcept
{
	parent_sigill_count = 1;
}

void supported_probe() noexcept
{
	asm volatile("" ::: "memory");
}

void unsupported_probe() noexcept
{
#if defined(__APPLE__) && defined(__aarch64__)
	asm volatile(".inst 0x00000000");
#else
	::raise(SIGILL);
#endif
}

bool same_handler(
	const struct sigaction &left, const struct sigaction &right) noexcept
{
	if (left.sa_handler != right.sa_handler ||
	    left.sa_flags != right.sa_flags)
		return false;
	for (int signal = 1; signal < NSIG; ++signal) {
		if (sigismember(&left.sa_mask, signal) !=
		    sigismember(&right.sa_mask, signal))
			return false;
	}
	return true;
}

bool same_mask(const sigset_t &left, const sigset_t &right) noexcept
{
	for (int signal = 1; signal < NSIG; ++signal) {
		if (sigismember(&left, signal) != sigismember(&right, signal))
			return false;
	}
	return true;
}

} // namespace

int main()
{
	using uml::backend::ArmFeature;
	using uml::backend::HostCapabilityResult;

	struct sigaction original_action {};
	sigset_t original_mask;
	if (::sigaction(SIGILL, nullptr, &original_action) != 0 ||
	    ::sigprocmask(SIG_SETMASK, nullptr, &original_mask) != 0)
		return EXIT_FAILURE;

	struct sigaction test_action {};
	test_action.sa_handler = parent_sigill_handler;
	if (sigemptyset(&test_action.sa_mask) != 0 ||
	    ::sigaction(SIGILL, &test_action, nullptr) != 0)
		return EXIT_FAILURE;

	sigset_t blocked_mask = original_mask;
	if (sigaddset(&blocked_mask, SIGILL) != 0 ||
	    ::sigprocmask(SIG_SETMASK, &blocked_mask, nullptr) != 0) {
		::sigaction(SIGILL, &original_action, nullptr);
		return EXIT_FAILURE;
	}

	bool passed = true;
	passed &= uml::backend::probe_host_instruction(nullptr) ==
		HostCapabilityResult::unavailable;
#if defined(__APPLE__) && defined(__aarch64__)
	passed &= uml::backend::probe_host_instruction(supported_probe) ==
		HostCapabilityResult::supported;
	passed &= uml::backend::probe_host_instruction(unsupported_probe) ==
		HostCapabilityResult::unsupported;
#else
	passed &= uml::backend::probe_host_instruction(supported_probe) ==
		HostCapabilityResult::unavailable;
	passed &= uml::backend::probe_host_instruction(unsupported_probe) ==
		HostCapabilityResult::unavailable;
#endif

	struct sigaction current_action {};
	sigset_t current_mask;
	passed &= ::sigaction(SIGILL, nullptr, &current_action) == 0;
	passed &= ::sigprocmask(SIG_SETMASK, nullptr, &current_mask) == 0;
	passed &= same_handler(current_action, test_action);
	passed &= same_mask(current_mask, blocked_mask);
	passed &= parent_sigill_count == 0;

	constexpr auto missing_feature =
		"hw.optional.arm.FEAT_CODEX_TEST_MISSING";
	passed &= uml::backend::query_host_arm_feature_sysctl(missing_feature) ==
		HostCapabilityResult::unavailable;
	passed &= !uml::backend::query_host_arm_feature(missing_feature);

	const auto lor_sysctl = uml::backend::query_host_arm_feature_sysctl(
		"hw.optional.arm.FEAT_LOR");
	const bool expected_lor =
		lor_sysctl == HostCapabilityResult::supported ||
		(lor_sysctl == HostCapabilityResult::unavailable &&
		 uml::backend::probe_host_instruction(
			 uml::backend::detail::instruction_probe_for(
				 ArmFeature::lor)) ==
			 HostCapabilityResult::supported);
	passed &= uml::backend::query_host_arm_feature(ArmFeature::lor) ==
		expected_lor;

	const bool restored_action =
		::sigaction(SIGILL, &original_action, nullptr) == 0;
	const bool restored_mask =
		::sigprocmask(SIG_SETMASK, &original_mask, nullptr) == 0;
	return passed && restored_action && restored_mask
		? EXIT_SUCCESS
		: EXIT_FAILURE;
}
