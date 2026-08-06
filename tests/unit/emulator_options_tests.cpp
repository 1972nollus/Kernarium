/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "options.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string_view>

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
		3, arguments, "/tmp/rootfs.img", true);
	require(options.valid, "valid emulator options were rejected");
	require(options.engine ==
			uml::runtime::ExecutionEngine::interpreter,
		"execution engine was not parsed");
	require(contains(options, "console=ttyS0"), "kernel argument was lost");
	require(contains(options, "ubd0r=/tmp/rootfs.img"),
		"rootfs default was not added");
	require(contains(options, "root=/dev/ubda1"), "root default was not added");
	require(contains(options, "init=/sbin/uml-overlay-init"), "init default was not added");
	require(contains(options, "mem=256M"), "memory default was not added");
	require(contains(options, "ip=dhcp"), "network default was not added");
	require(contains(options, "ro"), "read-only default was not added");
	require(!contains(options, "--execution-engine=interpreter"),
		"host execution option leaked into the kernel command line");
}

void test_explicit_defaults_are_preserved()
{
	const char *arguments[]{"console=ttyS0", "ubd0r=/custom/rootfs",
		"root=/dev/custom", "init=/custom-init", "mem=512M", "ip=dhcp",
		"ro", "--execution-engine", "gadgets"};
	const auto options = uml::emulator::prepare_emulator_options(
		9, arguments, "/unused/rootfs", true);
	require(options.valid, "explicit emulator options were rejected");
	require(options.engine == uml::runtime::ExecutionEngine::gadgets,
		"separate engine argument was not parsed");
	require(options.arguments.size() == 7,
		"explicit kernel policy values were duplicated");
	require(!contains(options, "rw"), "read-write policy overrode read-only");

	const char *writable_mapping[]{"ubd0=/custom/rootfs"};
	require(!uml::emulator::prepare_emulator_options(
			1, writable_mapping, "/unused/rootfs", false)
			.valid,
		"writable rootfs mapping was accepted");
	for (const char *mapping : {
		     "ubd0s=/custom/rootfs", "ubd0c=/custom/rootfs",
		     "ubd0d=/custom/rootfs", "ubd0t=/custom/rootfs",
		     "ubd0"}) {
		const char *flagged_mapping[]{mapping};
		require(!uml::emulator::prepare_emulator_options(
				1, flagged_mapping, "/unused/rootfs", false)
				.valid,
			"writable or malformed flagged rootfs mapping was accepted");
	}
	const char *flagged_read_only_mapping[]{"ubd0rscdt=/custom/rootfs"};
	require(uml::emulator::prepare_emulator_options(
			1, flagged_read_only_mapping, "/unused/rootfs", false)
			.valid,
		"read-only flagged rootfs mapping was rejected");
	const char *writable_root[]{"rw"};
	require(!uml::emulator::prepare_emulator_options(
			1, writable_root, "/unused/rootfs", false)
			.valid,
		"writable root mount was accepted");
}

void test_invalid_and_informational_inputs()
{
	const char *invalid_engine[]{"--execution-engine=unknown"};
	require(!uml::emulator::prepare_emulator_options(
			1, invalid_engine, "/tmp/rootfs.img", false)
			.valid,
		"unknown execution engine was accepted");
	const char *missing_value[]{"--execution-engine"};
	require(!uml::emulator::prepare_emulator_options(
			1, missing_value, "/tmp/rootfs.img", false)
			.valid,
		"missing execution engine value was accepted");
	const char *removed_option[]{"--jit-gadget-fallback=off"};
	require(!uml::emulator::prepare_emulator_options(
			1, removed_option, "/tmp/rootfs.img", false)
			.valid,
		"removed fallback option was accepted");
	const char *help[]{"--help"};
	const auto informational = uml::emulator::prepare_emulator_options(
		1, help, {}, false);
	require(informational.valid && informational.arguments.size() == 1,
		"informational invocation incorrectly required a rootfs");
	require(!uml::emulator::prepare_emulator_options(
			1, static_cast<const char *const *>(nullptr), {}, false)
			.valid,
		"null argument vector was accepted");
	const char *placeholder[]{"unused"};
	require(!uml::emulator::prepare_emulator_options(
			std::numeric_limits<int>::max(), placeholder,
			"/tmp/rootfs.img", false).valid,
		"argument count that overflows the kernel-entry ABI was accepted");
}

} // namespace

int main()
{
	try {
		test_defaults_and_engine_selection();
		test_explicit_defaults_are_preserved();
		test_invalid_and_informational_inputs();
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		return std::fprintf(stderr, "emulator options test failed: %s\n",
			error.what()), EXIT_FAILURE;
	}
}
