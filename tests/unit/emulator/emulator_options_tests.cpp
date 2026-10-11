/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mach_vm_region_test_support.hpp"
#include "options.hpp"
#include "require.hpp"
#include "runtime/native_kernel_layout.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>

namespace {

using uml::testing::require;

bool contains(const uml::emulator::EmulatorOptions &options,
		      std::string_view value)
{
	return std::ranges::any_of(options.arguments, [&](const auto &argument) {
		return argument == value;
	});
}

std::size_t count_prefixed(const uml::emulator::EmulatorOptions &options,
		      std::string_view prefix)
{
	return static_cast<std::size_t>(std::ranges::count_if(
		options.arguments, [&](const auto &argument) {
			return std::string_view{argument}.starts_with(prefix);
		}));
}

bool starts_with_any(const uml::emulator::EmulatorOptions &options,
		      std::string_view prefix)
{
	return count_prefixed(options, prefix) != 0U;
}

void test_defaults_and_engine_selection()
{
	const char *arguments[]{"uml-test", "--execution-engine=jit",
		"--guest-top-byte-ignore", "console=ttyS0"};
	const auto options = uml::emulator::prepare_emulator_options(
		4, arguments, "/tmp/rootfs.erofs");
	require(options.valid, "valid emulator options were rejected");
	require(options.engine == uml::runtime::ExecutionEngine::jit,
		"execution engine was not parsed");
	require(options.top_byte_ignore,
		"guest top-byte-ignore mode was not parsed");
	require(!contains(options, "--guest-top-byte-ignore"),
		"guest top-byte-ignore host option reached the kernel command line");
	require(contains(options, "console=ttyS0"), "kernel argument was lost");
	require(contains(options, "ubd0c=/tmp/rootfs.erofs"),
		"rootfs default was not added");
	require(contains(options, "root=/dev/ubda"), "root default was not added");
	require(contains(options, "rootfstype=erofs"),
		"EROFS root type was not added");
	/* The launcher owns the boot: an initramfs /init, or an init= it names
	 * itself. The library adds no init of its own. */
	require(!starts_with_any(options, "init="),
		"the library chose an init for the guest");
	require(contains(options, "mem=256M"), "memory default was not added");
	require(options.jit_cache_budget == 128U * 1024U * 1024U,
		"default memory did not produce the default half-memory JIT budget");
	require(options.guest_memory_bytes == 256U * 1024U * 1024U,
		"default guest memory was not retained for kernel placement");
	require(options.jit_executor_cache_budget == 256U * 1024U * 1024U,
		"default private JIT cache cap is not 256 MiB");
	/* No maintained kernel builds IP_PNP; guest userspace owns DHCP. */
	require(!starts_with_any(options, "ip="),
		"an inert kernel IP autoconfiguration argument was added");
	require(!contains(options, "--execution-engine=jit"),
		"host execution option leaked into the kernel command line");

	const char *default_arguments[]{"uml-test"};
	const auto defaults = uml::emulator::prepare_emulator_options(
		1, default_arguments, "/tmp/rootfs.erofs");
	require(contains(defaults, "console=tty0") &&
		contains(defaults, "console=ttyS0"),
		"framebuffer and host serial console defaults were not added");

	const char *disk_composed[]{"ubd0=/tmp/guest.img", "root=/dev/ram0"};
	const auto disk_options = uml::emulator::prepare_emulator_options(
		2, disk_composed, {});
	require(disk_options.valid,
		"rootfs-less disk-composed guest options were rejected");
	require(!starts_with_any(disk_options, "ip="),
		"rootfs-less guest inherited kernel DHCP configuration");
}

void test_explicit_defaults_are_preserved()
{
	const char *arguments[]{"console=ttyS0", "ubd0c=/custom/rootfs.erofs",
		"root=/dev/custom", "rootfstype=erofs", "init=/custom-init",
		"mem=512M", "ip=dhcp", "--execution-engine", "gadgets"};
	const auto options = uml::emulator::prepare_emulator_options(
		9, arguments, "/unused/rootfs");
	require(options.valid, "explicit emulator options were rejected");
	require(!options.top_byte_ignore,
		"guest top-byte-ignore mode was enabled by default");
	require(options.engine == uml::runtime::ExecutionEngine::gadgets,
		"separate engine argument was not parsed");
	/* The seven explicit values, plus the management-console name and
	 * directory the guest needs and the caller did not supply. */
	require(options.arguments.size() == 9,
		"explicit kernel policy values were duplicated");
	require(starts_with_any(options, "umid=") &&
		starts_with_any(options, "uml_dir="),
		"the guest was left without a management-console location");
	require(options.host_console_directory,
		"the host's console directory was not left to the run");
	const char *named_console[]{"umid=chosen"};
	const auto named = uml::emulator::prepare_emulator_options(
		1, named_console, "/tmp/rootfs.erofs");
	require(contains(named, "umid=chosen") &&
		count_prefixed(named, "umid=") == 1,
		"a caller-supplied management-console name was replaced");
	/* The console's path does not use the umid when uml_dir= names a
	 * directory, so the host's directory still applies. */
	require(count_prefixed(named, "uml_dir=") == 1 &&
		named.host_console_directory,
		"a caller-supplied name displaced the host's console directory");
	const char *placed_console[]{"uml_dir=/chosen"};
	const auto placed = uml::emulator::prepare_emulator_options(
		1, placed_console, "/tmp/rootfs.erofs");
	require(contains(placed, "uml_dir=/chosen") &&
		count_prefixed(placed, "uml_dir=") == 1,
		"a caller-supplied management-console directory was replaced");
	require(!placed.host_console_directory,
		"the run would prepare a directory the guest does not use");
	const char *explicit_mapping[]{"ubd0c=/custom/rootfs.erofs"};
	require(uml::emulator::prepare_emulator_options(
			1, explicit_mapping, "/unused/rootfs.erofs")
			.valid,
		"explicit EROFS mapping was rejected");
	/* The rootfs is named in ubd0c=, which the kernel would split at the
	 * space or misread at the comma. */
	const char *plain[]{"uml-test"};
	require(!uml::emulator::prepare_emulator_options(
			1, plain, "/tmp/My Disk.erofs").valid &&
		!uml::emulator::prepare_emulator_options(
			1, plain, "/tmp/a,b.erofs").valid,
		"a rootfs path the UBD argument cannot carry was accepted");
}

void test_x86_64_kernel_memory_accounting_policy()
{
#if DARWIN_UML_X86_64_USERSPACE_ONLY
	const char *defaults[]{"uml-test"};
	const auto options = uml::emulator::prepare_emulator_options(
		1, defaults, "/tmp/rootfs.erofs");
	require(contains(options, "cgroup.memory=nokmem"),
		"x86_64 userspace slice enabled kernel-object memory charging");

	const char *explicit_policy[]{"uml-test", "cgroup.memory=default"};
	const auto explicit_options = uml::emulator::prepare_emulator_options(
		2, explicit_policy, "/tmp/rootfs.erofs");
	require(contains(explicit_options, "cgroup.memory=default") &&
		count_prefixed(explicit_options, "cgroup.memory=") == 1,
		"explicit cgroup memory policy was replaced or duplicated");
#endif
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
			2, arguments, "/tmp/rootfs.erofs");
		require(options.valid, "kernel-compatible mem= value was rejected");
		require(options.jit_cache_budget == example.expected_budget,
			"mem= value did not match kernel memparse semantics");
		require(options.guest_memory_bytes == example.expected_budget * 2U,
			"kernel placement memory did not match memparse semantics");
	}

	const char *repeated[]{"uml-test", "mem=1G", "mem=512M"};
	auto options = uml::emulator::prepare_emulator_options(
		3, repeated, "/tmp/rootfs.erofs");
	require(options.jit_cache_budget == 256U * mib,
		"the last repeated mem= value did not own the JIT budget");

	const char *last_malformed[]{"uml-test", "mem=1G", "mem=bogus"};
	options = uml::emulator::prepare_emulator_options(
		3, last_malformed, "/tmp/rootfs.erofs");
	require(options.jit_cache_budget == 0,
		"a final malformed mem= did not replace the earlier kernel value");

	const char *invalid_octal[]{"uml-test", "mem=09G"};
	options = uml::emulator::prepare_emulator_options(
		2, invalid_octal, "/tmp/rootfs.erofs");
	require(options.jit_cache_budget == 0,
		"base-zero octal parsing did not stop at the invalid digit");

	const char *strict_host_suffix[]{"uml-test", "--jit-cache-budget=1T"};
	require(!uml::emulator::prepare_emulator_options(
		2, strict_host_suffix, "/tmp/rootfs.erofs").valid,
		"host JIT budget unexpectedly inherited the guest's T suffix");
	const char *strict_host_trailing[]{"uml-test",
		"--jit-cache-budget=1Gignored"};
	require(!uml::emulator::prepare_emulator_options(
		2, strict_host_trailing, "/tmp/rootfs.erofs").valid,
		"host JIT budget unexpectedly ignored trailing text");

	const char *private_cap[]{"uml-test",
		"--jit-executor-cache-budget=32M"};
	options = uml::emulator::prepare_emulator_options(
		2, private_cap, "/tmp/rootfs.erofs");
	require(options.valid &&
		options.jit_executor_cache_budget == 32U * mib &&
		!contains(options, private_cap[1]),
		"private JIT cache cap was not consumed by the host");
	const char *maximum_private_cap[]{"uml-test",
		"--jit-executor-cache-budget=256M"};
	options = uml::emulator::prepare_emulator_options(
		2, maximum_private_cap, "/tmp/rootfs.erofs");
	require(options.valid &&
		options.jit_executor_cache_budget == 256U * mib,
		"maximum private JIT cache cap was rejected");
	const char *oversized_private_cap[]{"uml-test",
		"--jit-executor-cache-budget=257M"};
	require(!uml::emulator::prepare_emulator_options(
		2, oversized_private_cap, "/tmp/rootfs.erofs").valid,
		"private JIT cache cap above 256 MiB was accepted");
}

void test_jit_tier_option()
{
	const char *absent[]{"uml-test"};
	require(uml::emulator::prepare_emulator_options(
		1, absent, "/tmp/rootfs.erofs").jit_tier_instructions == 0,
		"the JIT tier defaulted to something other than off");
	const char *tiered[]{"uml-test", "--jit-tier=200000000",
		"ip=dhcp"};
	auto options = uml::emulator::prepare_emulator_options(
		3, tiered, "/tmp/rootfs.erofs");
	require(options.valid && options.jit_tier_instructions == 200000000ULL &&
		!contains(options, tiered[1]) && contains(options, "ip=dhcp"),
		"the JIT tier was not consumed by the host");
	const char *zero[]{"uml-test", "--jit-tier=0"};
	options = uml::emulator::prepare_emulator_options(
		2, zero, "/tmp/rootfs.erofs");
	require(options.valid && options.jit_tier_instructions == 0,
		"a zero JIT tier was rejected");
	const char *repeated[]{"uml-test", "--jit-tier=5", "--jit-tier=7"};
	options = uml::emulator::prepare_emulator_options(
		3, repeated, "/tmp/rootfs.erofs");
	require(options.valid && options.jit_tier_instructions == 7,
		"the last repeated JIT tier did not win");
	for (const char *invalid : {"--jit-tier=", "--jit-tier=20M",
		"--jit-tier=-1", "--jit-tier=1 ", "--jit-tier=0x10",
		"--jit-tier=99999999999999999999"}) {
		const char *arguments[]{"uml-test", invalid};
		require(!uml::emulator::prepare_emulator_options(
			2, arguments, "/tmp/rootfs.erofs").valid,
			"a malformed JIT tier was accepted");
	}
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
	static_assert(DARWIN_UML_NATIVE_AUXILIARY_MINIMUM == gib);
	static_assert(DARWIN_UML_NATIVE_AUXILIARY_MAXIMUM == 4ULL * gib);
	static_assert(DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION ==
		gib + 32ULL * kib);
	static_assert(DARWIN_UML_NATIVE_STUB_PAGES == 3);
	/* The kernel places its stub pages and the guarded shared-memory
	 * aperture above the RAM-scaled allowance, so the admitted window
	 * reserves the same span for every page profile. */
	constexpr std::uint64_t reserved_16k =
		DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION +
		DARWIN_UML_NATIVE_STUB_PAGES * 16ULL * kib;
	constexpr std::uint64_t reserved_4k =
		DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION +
		DARWIN_UML_NATIVE_STUB_PAGES * 4ULL * kib;
	constexpr std::uint64_t product_reserved =
		DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION +
		DARWIN_UML_NATIVE_STUB_PAGES * DARWIN_UML_GUEST_PAGE_SIZE;
	using Geometry = uml::runtime::NativeKernelLayoutGeometry<
		48ULL * gib, 62ULL * gib, gib, 4ULL * gib, 16ULL * kib,
		DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION,
		DARWIN_UML_NATIVE_STUB_PAGES>;
	/* Auxiliary tracks RAM one-for-one between the floor and the ceiling,
	 * because vmalloc must never be the binding constraint; its pages are
	 * already bounded by the guest page allocator. */
	const auto one_gib = Geometry::derive(gib);
	require(one_gib.valid &&
		one_gib.window_start ==
			62ULL * gib - 2ULL * gib - reserved_16k &&
		one_gib.window_end == 62ULL * gib &&
		one_gib.load_bias == 12ULL * gib - reserved_16k,
		"1 GiB RAM did not produce a 1 GiB auxiliary window");
	const auto two_gib = Geometry::derive(2ULL * gib);
	require(two_gib.valid &&
		two_gib.window_end - two_gib.window_start ==
			4ULL * gib + reserved_16k,
		"2 GiB RAM did not track its auxiliary window one-for-one");
	const auto eight_gib = Geometry::derive(8ULL * gib);
	require(eight_gib.valid &&
		eight_gib.window_end - eight_gib.window_start ==
			12ULL * gib + reserved_16k &&
		eight_gib.window_start == 50ULL * gib - reserved_16k,
		"8 GiB RAM did not cap its auxiliary window at 4 GiB");
	const auto small = Geometry::derive(128ULL * mib);
	require(small.valid && small.window_end - small.window_start ==
		128ULL * mib + gib + reserved_16k,
		"small guest did not retain the 1 GiB auxiliary floor");
	constexpr std::uint64_t envelope_fill =
		14ULL * gib - 4ULL * gib - reserved_16k;
	const auto largest = Geometry::derive(envelope_fill);
	require(largest.valid &&
		largest.window_end - largest.window_start == 14ULL * gib &&
		largest.window_start == 48ULL * gib,
		"the largest guest did not exactly fill the signed envelope");
	require(!Geometry::derive(envelope_fill + 16ULL * kib).valid,
		"RAM and its capped auxiliary window exceeded the signed envelope");
	const auto rounded = Geometry::derive(1);
	require(rounded.valid && rounded.window_end - rounded.window_start ==
		gib + 16ULL * kib + reserved_16k,
		"native window did not page-round RAM or retain its floor");
	using FourKGeometry = uml::runtime::NativeKernelLayoutGeometry<
		48ULL * gib, 62ULL * gib, gib, 4ULL * gib, 4ULL * kib,
		DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION,
		DARWIN_UML_NATIVE_STUB_PAGES>;
	const auto four_kib_rounded = FourKGeometry::derive(gib + 1U);
	require(four_kib_rounded.valid &&
		four_kib_rounded.window_end - four_kib_rounded.window_start ==
			2ULL * gib + 8ULL * kib + reserved_4k,
		"4 KiB geometry did not independently round RAM and auxiliary space");
	using HighGeometry = uml::runtime::NativeKernelLayoutGeometry<
		48ULL * gib, 128ULL * 1024ULL * gib, gib, 4ULL * gib,
		16ULL * kib, DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION,
		DARWIN_UML_NATIVE_STUB_PAGES>;
	const auto high = HighGeometry::derive_at(
		8ULL * gib, 96ULL * 1024ULL * gib);
	require(high.valid &&
		high.window_start == 96ULL * 1024ULL * gib &&
		high.window_end - high.window_start ==
			12ULL * gib + reserved_16k &&
		high.load_bias == high.window_start - 48ULL * gib,
		"high native placement did not retain the scalable window");
	const auto product = uml::runtime::prepare_native_kernel_layout(gib);
#if DARWIN_UML_NATIVE_HIGH_PLACEMENT
	require(product.valid &&
		product.window_start == 96ULL * 1024ULL * gib &&
		product.window_end - product.window_start ==
			2ULL * gib + product_reserved &&
		product.load_bias == product.window_start - 48ULL * gib,
		"macOS did not select its high dynamic native window");
#else
	/* A physical device maps guest RAM over its window in place, so the
	 * window is always reserved: a failed reservation fails the run rather
	 * than fall back to the packed geometry. The host picks where it goes
	 * when it declines the packed address, as iOS 27 does. */
	require(product.valid && product.reserved &&
		product.window_end - product.window_start ==
			2ULL * gib + product_reserved &&
		product.load_bias ==
			product.window_start - DARWIN_UML_NATIVE_LINK_ADDRESS,
		"iOS did not reserve its dynamic native window");
#endif
}

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UML_OPTIONS_TEST_UNDER_ASAN 1
#endif
#endif

/* A reserved native window must reach the loader whole. Darwin places its
 * next unhinted allocation in a range freed inside it, which the guest's next
 * fixed mapping would then replace without telling either owner. */
void test_reserved_native_window_has_no_gap()
{
#if defined(UML_OPTIONS_TEST_UNDER_ASAN)
	/* AddressSanitizer keeps its allocator heap at 0x600000000000 on arm64
	 * macOS, the product's macOS native window, so the window cannot be
	 * reserved beside it. */
	std::puts("unsupported: the reserved native window has no gap: "
		"AddressSanitizer's heap occupies the macOS native window");
#else
	constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
	constexpr std::uintptr_t start = DARWIN_UML_MACOS_NATIVE_ADDRESS;
	constexpr std::uintptr_t length = 2U * gib +
		DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION +
		DARWIN_UML_NATIVE_STUB_PAGES * DARWIN_UML_GUEST_PAGE_SIZE;
	const auto window = uml::runtime::reserve_native_kernel_window({
		.load_bias = start - DARWIN_UML_NATIVE_LINK_ADDRESS,
		.window_start = start,
		.window_end = start + length,
		.valid = true,
	});
	require(window.valid && window.reserved &&
		window.window_start == start &&
		window.window_end == start + length &&
		window.load_bias == start - DARWIN_UML_NATIVE_LINK_ADDRESS,
		"the native window was not reserved where the host offered it");
	mach_vm_address_t cursor = window.window_start;
	while (cursor < window.window_end) {
		const auto query = uml::testing::query_vm_region(
			static_cast<std::uintptr_t>(cursor));
		require(query.status == KERN_SUCCESS &&
			query.region.start == cursor && query.region.size != 0,
			"the reserved native window has a gap");
		require(query.region.protection == VM_PROT_NONE,
			"the reserved native window is accessible");
		cursor = query.region.start + query.region.size;
	}
	require(munmap(reinterpret_cast<void *>(window.window_start),
		length) == 0, "the reserved native window could not be released");
	require(!uml::runtime::reserve_native_kernel_window({}).valid,
		"an invalid geometry reserved a native window");
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
		1, defaults, "/tmp/rootfs.erofs");
	require(options.valid, "default CPU topology was rejected");
	require(options.guest_cpus == expected_default,
		"absent ncpus did not follow the kernel's online-CPU default");

	const char *explicit_count[]{"uml-test", "ncpus=10"};
	options = uml::emulator::prepare_emulator_options(
		2, explicit_count, "/tmp/rootfs.erofs");
	require(options.guest_cpus == 10,
		"ordinary ncpus value was not preserved");

	const char *clamped_high[]{"uml-test", "ncpus=1024"};
	options = uml::emulator::prepare_emulator_options(
		2, clamped_high, "/tmp/rootfs.erofs");
	require(options.guest_cpus ==
			uml::runtime::maximum_guest_cpu_count,
		"ncpus was not clamped to the kernel's CONFIG_NR_CPUS");

	const char *clamped_low[]{"uml-test", "ncpus=0"};
	options = uml::emulator::prepare_emulator_options(
		2, clamped_low, "/tmp/rootfs.erofs");
	require(options.guest_cpus == 1,
		"zero ncpus was not clamped like the kernel");
	const char *signed_counts[]{"uml-test", "ncpus=-2", "ncpus=+6"};
	options = uml::emulator::prepare_emulator_options(
		3, signed_counts, "/tmp/rootfs.erofs");
	require(options.guest_cpus == 6,
		"signed ncpus values did not follow kernel integer parsing");

	const char *malformed[]{"uml-test", "ncpus=10junk"};
	options = uml::emulator::prepare_emulator_options(
		2, malformed, "/tmp/rootfs.erofs");
	require(options.guest_cpus == expected_default,
		"malformed ncpus changed the kernel-compatible default");
	const char *terminal_newline[]{"uml-test", "ncpus=7\n"};
	options = uml::emulator::prepare_emulator_options(
		2, terminal_newline, "/tmp/rootfs.erofs");
	require(options.guest_cpus == 7,
		"terminal-newline ncpus did not match kstrtoint");
	const char *malformed_newline[]{"uml-test", "ncpus=7\n\n"};
	options = uml::emulator::prepare_emulator_options(
		2, malformed_newline, "/tmp/rootfs.erofs");
	require(options.guest_cpus == expected_default,
		"multiple terminal newlines were accepted");

	const char *repeated[]{"uml-test", "ncpus=4", "ncpus=bogus",
		"ncpus=7"};
	options = uml::emulator::prepare_emulator_options(
		4, repeated, "/tmp/rootfs.erofs");
	require(options.guest_cpus == 7,
		"later valid ncpus did not match kernel argument ordering");
}

void test_invalid_and_informational_inputs()
{
	const char *invalid_engine[]{"--execution-engine=unknown"};
	require(!uml::emulator::prepare_emulator_options(
		1, invalid_engine, "/tmp/rootfs.erofs")
			.valid,
		"unknown execution engine was accepted");
	const char *missing_value[]{"--execution-engine"};
	require(!uml::emulator::prepare_emulator_options(
		1, missing_value, "/tmp/rootfs.erofs")
			.valid,
		"missing execution engine value was accepted");
	const char *removed_option[]{"--jit-gadget-fallback=off"};
	require(!uml::emulator::prepare_emulator_options(
		1, removed_option, "/tmp/rootfs.erofs")
			.valid,
		"removed fallback option was accepted");
	const char *help[]{"--help"};
	const auto informational = uml::emulator::prepare_emulator_options(
		1, help, {});
	require(informational.valid && informational.informational &&
		informational.arguments.size() == 1 &&
		informational.guest_memory_bytes == 64U * 1024U * 1024U,
		"informational invocation incorrectly required a rootfs");
	require(!uml::emulator::prepare_emulator_options(
			1, static_cast<const char *const *>(nullptr), {})
			.valid,
		"null argument vector was accepted");
	const char *placeholder[]{"unused"};
	require(!uml::emulator::prepare_emulator_options(
			std::numeric_limits<int>::max(), placeholder,
			"/tmp/rootfs.erofs").valid,
		"argument count that overflows the kernel-entry ABI was accepted");
}

} // namespace

int main()
{
	try {
		test_defaults_and_engine_selection();
		test_explicit_defaults_are_preserved();
		test_x86_64_kernel_memory_accounting_policy();
		test_guest_memory_budget_matches_kernel_memparse();
		test_jit_tier_option();
		test_scalable_native_kernel_layout_geometry();
		test_reserved_native_window_has_no_gap();
		test_guest_cpu_count_matches_kernel_policy();
		test_invalid_and_informational_inputs();
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		return std::fprintf(stderr, "emulator options test failed: %s\n",
			error.what()), EXIT_FAILURE;
	}
}
