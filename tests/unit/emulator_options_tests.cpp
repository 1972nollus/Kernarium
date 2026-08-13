/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "options.hpp"
#include "runtime/native_kernel_layout.hpp"

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
	require(options.guest_memory_bytes == 256U * 1024U * 1024U,
		"default guest memory was not retained for kernel placement");
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
		require(options.guest_memory_bytes == example.expected_budget * 2U,
			"kernel placement memory did not match memparse semantics");
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

void test_scalable_native_kernel_layout_geometry()
{
	constexpr std::uint64_t kib = 1024ULL;
	constexpr std::uint64_t mib = kib * 1024ULL;
	constexpr std::uint64_t gib = mib * 1024ULL;
	static_assert(DARWIN_UML_GUEST_ADDRESS_LIMIT == 128ULL * 1024ULL * gib);
	static_assert(DARWIN_UML_NATIVE_ADDRESS_START == 48ULL * gib);
	static_assert(DARWIN_UML_NATIVE_ADDRESS_END == 62ULL * gib);
	static_assert(DARWIN_UML_NATIVE_LINK_ADDRESS == 48ULL * gib);
	static_assert(DARWIN_UML_NATIVE_ADDRESS_CEILING == 62ULL * gib);
	static_assert(DARWIN_UML_MACOS_NATIVE_ADDRESS == 96ULL * 1024ULL * gib);
	static_assert(DARWIN_UML_NATIVE_AUXILIARY_DIVISOR == 8);
	static_assert(DARWIN_UML_NATIVE_AUXILIARY_MINIMUM == 32ULL * mib);
	static_assert(DARWIN_UML_DYNAMIC_NATIVE_LAYOUT == 1);
	using Geometry = uml::runtime::NativeKernelLayoutGeometry<
		48ULL * gib, 62ULL * gib, 8, 32ULL * mib, 16ULL * kib>;
	const auto one_gib = Geometry::derive(gib);
	require(one_gib.valid &&
		one_gib.window_start == 62ULL * gib - gib - 128ULL * mib &&
		one_gib.window_end == 62ULL * gib &&
		one_gib.load_bias == 12ULL * gib + 896ULL * mib,
		"1 GiB RAM did not produce an exact 128 MiB auxiliary window");
	const auto two_gib = Geometry::derive(2ULL * gib);
	require(two_gib.valid &&
		two_gib.window_end - two_gib.window_start ==
			2ULL * gib + 256ULL * mib,
		"2 GiB RAM did not scale the auxiliary window to 256 MiB");
	const auto eight_gib = Geometry::derive(8ULL * gib);
	require(eight_gib.valid &&
		eight_gib.window_end - eight_gib.window_start == 9ULL * gib &&
		eight_gib.window_start == 53ULL * gib,
		"the iOS geometry did not produce an 8 GiB 53--62 GiB window");
	const auto small = Geometry::derive(128ULL * mib);
	require(small.valid && small.window_end - small.window_start ==
		128ULL * mib + 32ULL * mib,
		"small guest did not retain Linux UML's 32 MiB minimum");
	const auto twelve_gib = Geometry::derive(12ULL * gib);
	require(twelve_gib.valid &&
		twelve_gib.window_end - twelve_gib.window_start ==
			13ULL * gib + 512ULL * mib,
		"large guest did not retain the one-eighth auxiliary ratio");
	require(!Geometry::derive(13ULL * gib).valid,
		"RAM and its scaled auxiliary window exceeded the signed envelope");
	const auto rounded = Geometry::derive(1);
	require(rounded.valid && rounded.window_end - rounded.window_start ==
		32ULL * mib + 16ULL * kib,
		"native window did not page-round RAM or retain its minimum");
	using FourKGeometry = uml::runtime::NativeKernelLayoutGeometry<
		48ULL * gib, 62ULL * gib, 8, 32ULL * mib, 4ULL * kib>;
	const auto four_kib_rounded = FourKGeometry::derive(gib + 1U);
	require(four_kib_rounded.valid &&
		four_kib_rounded.window_end - four_kib_rounded.window_start ==
			gib + 128ULL * mib + 8ULL * kib,
		"4 KiB geometry did not independently round RAM and auxiliary space");
	using HighGeometry = uml::runtime::NativeKernelLayoutGeometry<
		48ULL * gib, 128ULL * 1024ULL * gib, 8, 32ULL * mib,
		16ULL * kib>;
	const auto high = HighGeometry::derive_at(
		8ULL * gib, 96ULL * 1024ULL * gib);
	require(high.valid &&
		high.window_start == 96ULL * 1024ULL * gib &&
		high.window_end - high.window_start == 9ULL * gib &&
		high.load_bias == high.window_start - 48ULL * gib,
		"high native placement did not retain the scalable window");
	const auto product = uml::runtime::prepare_native_kernel_layout(gib);
#if DARWIN_UML_NATIVE_HIGH_PLACEMENT
	require(product.valid &&
		product.window_start == 96ULL * 1024ULL * gib &&
		product.window_end - product.window_start == gib + 128ULL * mib &&
		product.load_bias == product.window_start - 48ULL * gib,
		"macOS did not select its high dynamic native window");
#else
	require(product.valid &&
		product.window_start == one_gib.window_start &&
		product.window_end == one_gib.window_end &&
		product.load_bias == one_gib.load_bias,
		"iOS did not select its low dynamic native window");
#endif
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
		informational.arguments.size() == 1 &&
		informational.guest_memory_bytes == 64U * 1024U * 1024U,
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
		test_scalable_native_kernel_layout_geometry();
		test_guest_cpu_count_matches_kernel_policy();
		test_invalid_and_informational_inputs();
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		return std::fprintf(stderr, "emulator options test failed: %s\n",
			error.what()), EXIT_FAILURE;
	}
}
