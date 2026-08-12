/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "options.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace {

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error{message};
}

bool contains(const uml::emulator::EmulatorOptions &options,
		      std::string_view value)
{
	return std::ranges::any_of(options.arguments, [&](const auto &argument) {
		return argument == value;
	});
}

void test_defaults_and_engine_selection()
{
	const char *arguments[]{"uml-test", "--execution-engine=interpreter",
		"console=ttyS0"};
	const auto options = uml::emulator::prepare_emulator_options(
		3, arguments, "/tmp/rootfs.erofs", true);
	require(options.valid, "valid emulator options were rejected");
	require(options.engine ==
			uml::runtime::ExecutionEngine::interpreter,
		"execution engine was not parsed");
	require(contains(options, "console=ttyS0"), "kernel argument was lost");
	require(contains(options, "ubd0c=/tmp/rootfs.erofs"),
		"rootfs default was not added");
	require(contains(options, "root=/dev/ubda"), "root default was not added");
	require(contains(options, "rootfstype=erofs"),
		"EROFS root type was not added");
	require(contains(options, "init=/bin/sh"),
		"interactive shell init was not added");
	require(!contains(options, "init=/sbin/uml-overlay-init"),
		"removed image-local overlay init was injected");
	require(contains(options, "mem=256M"), "memory default was not added");
	require(options.jit_cache_budget == 128U * 1024U * 1024U,
		"default memory did not produce the default half-memory JIT budget");
	require(options.jit_executor_cache_budget == 64U * 1024U * 1024U,
		"default private JIT cache cap is not 64 MiB");
	require(contains(options, "ip=dhcp"), "network default was not added");
	require(!contains(options, "--execution-engine=interpreter"),
		"host execution option leaked into the kernel command line");
}

void test_explicit_defaults_are_preserved()
{
	const char *arguments[]{"console=ttyS0", "ubd0c=/custom/rootfs.erofs",
		"root=/dev/custom", "rootfstype=erofs", "init=/custom-init",
		"mem=512M", "ip=dhcp", "--execution-engine", "gadgets"};
	const auto options = uml::emulator::prepare_emulator_options(
		9, arguments, "/unused/rootfs", true);
	require(options.valid, "explicit emulator options were rejected");
	require(options.engine == uml::runtime::ExecutionEngine::gadgets,
		"separate engine argument was not parsed");
	require(options.arguments.size() == 7,
		"explicit kernel policy values were duplicated");
	const char *explicit_mapping[]{"ubd0c=/custom/rootfs.erofs"};
	require(uml::emulator::prepare_emulator_options(
			1, explicit_mapping, "/unused/rootfs.erofs", false)
			.valid,
		"explicit EROFS mapping was rejected");
}

void test_guest_memory_budget_matches_kernel_memparse()
{
	constexpr std::size_t kib = 1024U;
	constexpr std::size_t mib = kib * 1024U;
	constexpr std::size_t gib = mib * 1024U;
	struct Example final {
		const char *argument;
		std::size_t expected_budget;
	};
	constexpr std::array examples{
		Example{"mem=1K", kib / 2U},
		Example{"mem=2m", mib},
		Example{"mem=1G", gib / 2U},
		Example{"mem=1t", std::size_t{1} << 39U},
		Example{"mem=1P", std::size_t{1} << 49U},
		Example{"mem=1e", std::size_t{1} << 59U},
		Example{"mem=0x40000000", gib / 2U},
		Example{"mem=010000000000", gib / 2U},
		Example{"mem=1Gignored-by-uml-setup", gib / 2U},
		Example{"mem=1KB", kib / 2U},
	};
	for (const auto &example : examples) {
		const char *arguments[]{"uml-test", example.argument};
		const auto options = uml::emulator::prepare_emulator_options(
			2, arguments, "/tmp/rootfs.erofs", false);
		require(options.valid, "kernel-compatible mem= value was rejected");
		require(options.jit_cache_budget == example.expected_budget,
			"mem= value did not match kernel memparse semantics");
	}

	const char *repeated[]{"uml-test", "mem=1G", "mem=512M"};
	auto options = uml::emulator::prepare_emulator_options(
		3, repeated, "/tmp/rootfs.erofs", false);
	require(options.jit_cache_budget == 256U * mib,
		"the last repeated mem= value did not own the JIT budget");

	const char *last_malformed[]{"uml-test", "mem=1G", "mem=bogus"};
	options = uml::emulator::prepare_emulator_options(
		3, last_malformed, "/tmp/rootfs.erofs", false);
	require(options.jit_cache_budget == 0,
		"a final malformed mem= did not replace the earlier kernel value");

	const char *invalid_octal[]{"uml-test", "mem=09G"};
	options = uml::emulator::prepare_emulator_options(
		2, invalid_octal, "/tmp/rootfs.erofs", false);
	require(options.jit_cache_budget == 0,
		"base-zero octal parsing did not stop at the invalid digit");

	const char *strict_host_suffix[]{"uml-test", "--jit-cache-budget=1T"};
	require(!uml::emulator::prepare_emulator_options(
		2, strict_host_suffix, "/tmp/rootfs.erofs", false).valid,
		"host JIT budget unexpectedly inherited the guest's T suffix");
	const char *strict_host_trailing[]{"uml-test",
		"--jit-cache-budget=1Gignored"};
	require(!uml::emulator::prepare_emulator_options(
		2, strict_host_trailing, "/tmp/rootfs.erofs", false).valid,
		"host JIT budget unexpectedly ignored trailing text");

	const char *private_cap[]{"uml-test",
		"--jit-executor-cache-budget=32M"};
	options = uml::emulator::prepare_emulator_options(
		2, private_cap, "/tmp/rootfs.erofs", false);
	require(options.valid &&
		options.jit_executor_cache_budget == 32U * mib &&
		!contains(options, private_cap[1]),
		"private JIT cache cap was not consumed by the host");
	const char *oversized_private_cap[]{"uml-test",
		"--jit-executor-cache-budget=65M"};
	require(!uml::emulator::prepare_emulator_options(
		2, oversized_private_cap, "/tmp/rootfs.erofs", false).valid,
		"private JIT cache cap above 64 MiB was accepted");
}

void test_guest_cpu_count_matches_kernel_policy()
{
	const auto online = sysconf(_SC_NPROCESSORS_ONLN);
	const auto expected_default = static_cast<unsigned>(std::clamp<long>(
		online > 0 ? online : 1, 1,
		static_cast<long>(
			uml::runtime::maximum_guest_cpu_count)));
	const char *defaults[]{"uml-test"};
	auto options = uml::emulator::prepare_emulator_options(
		1, defaults, "/tmp/rootfs.erofs", false);
	require(options.valid, "default CPU topology was rejected");
	require(options.guest_cpus == expected_default,
		"absent ncpus did not follow the kernel's online-CPU default");

	const char *explicit_count[]{"uml-test", "ncpus=10"};
	options = uml::emulator::prepare_emulator_options(
		2, explicit_count, "/tmp/rootfs.erofs", false);
	require(options.guest_cpus == 10,
		"ordinary ncpus value was not preserved");

	const char *clamped_high[]{"uml-test", "ncpus=1024"};
	options = uml::emulator::prepare_emulator_options(
		2, clamped_high, "/tmp/rootfs.erofs", false);
	require(options.guest_cpus ==
			uml::runtime::maximum_guest_cpu_count,
		"ncpus was not clamped to the kernel's CONFIG_NR_CPUS");

	const char *clamped_low[]{"uml-test", "ncpus=0"};
	options = uml::emulator::prepare_emulator_options(
		2, clamped_low, "/tmp/rootfs.erofs", false);
	require(options.guest_cpus == 1,
		"zero ncpus was not clamped like the kernel");
	const char *signed_counts[]{"uml-test", "ncpus=-2", "ncpus=+6"};
	options = uml::emulator::prepare_emulator_options(
		3, signed_counts, "/tmp/rootfs.erofs", false);
	require(options.guest_cpus == 6,
		"signed ncpus values did not follow kernel integer parsing");

	const char *malformed[]{"uml-test", "ncpus=10junk"};
	options = uml::emulator::prepare_emulator_options(
		2, malformed, "/tmp/rootfs.erofs", false);
	require(options.guest_cpus == expected_default,
		"malformed ncpus changed the kernel-compatible default");
	const char *terminal_newline[]{"uml-test", "ncpus=7\n"};
	options = uml::emulator::prepare_emulator_options(
		2, terminal_newline, "/tmp/rootfs.erofs", false);
	require(options.guest_cpus == 7,
		"terminal-newline ncpus did not match kstrtoint");
	const char *malformed_newline[]{"uml-test", "ncpus=7\n\n"};
	options = uml::emulator::prepare_emulator_options(
		2, malformed_newline, "/tmp/rootfs.erofs", false);
	require(options.guest_cpus == expected_default,
		"multiple terminal newlines were accepted");

	const char *repeated[]{"uml-test", "ncpus=4", "ncpus=bogus",
		"ncpus=7"};
	options = uml::emulator::prepare_emulator_options(
		4, repeated, "/tmp/rootfs.erofs", false);
	require(options.guest_cpus == 7,
		"later valid ncpus did not match kernel argument ordering");
}

void test_invalid_and_informational_inputs()
{
	const char *invalid_engine[]{"--execution-engine=unknown"};
	require(!uml::emulator::prepare_emulator_options(
		1, invalid_engine, "/tmp/rootfs.erofs", false)
			.valid,
		"unknown execution engine was accepted");
	const char *missing_value[]{"--execution-engine"};
	require(!uml::emulator::prepare_emulator_options(
		1, missing_value, "/tmp/rootfs.erofs", false)
			.valid,
		"missing execution engine value was accepted");
	const char *removed_option[]{"--jit-gadget-fallback=off"};
	require(!uml::emulator::prepare_emulator_options(
		1, removed_option, "/tmp/rootfs.erofs", false)
			.valid,
		"removed fallback option was accepted");
	const char *help[]{"--help"};
	const auto informational = uml::emulator::prepare_emulator_options(
		1, help, {}, false);
	require(informational.valid && informational.informational &&
		informational.arguments.size() == 1,
		"informational invocation incorrectly required a rootfs");
	require(!uml::emulator::prepare_emulator_options(
			1, static_cast<const char *const *>(nullptr), {}, false)
			.valid,
		"null argument vector was accepted");
	const char *placeholder[]{"unused"};
	require(!uml::emulator::prepare_emulator_options(
			std::numeric_limits<int>::max(), placeholder,
			"/tmp/rootfs.erofs", false).valid,
		"argument count that overflows the kernel-entry ABI was accepted");
}

} // namespace

int main()
{
	try {
		test_defaults_and_engine_selection();
		test_explicit_defaults_are_preserved();
		test_guest_memory_budget_matches_kernel_memparse();
		test_guest_cpu_count_matches_kernel_policy();
		test_invalid_and_informational_inputs();
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		return std::fprintf(stderr, "emulator options test failed: %s\n",
			error.what()), EXIT_FAILURE;
	}
}
