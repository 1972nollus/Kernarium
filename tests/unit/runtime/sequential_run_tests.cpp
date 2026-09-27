// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A process runs one guest. A kernel claims guest RAM early in its boot, and
 * the host hands guest RAM out once per process. After a run has claimed it,
 * every later run in the process, on the same handle or a fresh one, returns
 * -UML_EMULATOR_UNSUPPORTED without entering a kernel, and the process goes
 * on. A run whose kernel returns before claiming guest RAM leaves the guest
 * for a later run.
 *
 * Each mode runs in its own process because the claim outlives every handle.
 */
#include "uml/emulator.h"
#include "uml_darwin_host.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace {

constexpr int kernel_booted = 41;
constexpr int kernel_without_memory = 42;
constexpr int kernel_left_memory = 43;

/* Kernel entries in this process, and whether the next one claims RAM. */
int kernel_entries;
bool claim_guest_ram = true;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error{message};
}

/* Stands in for the start of a UML boot: setup_physmem() opens guest RAM. */
int kernel(int, char *const[], char *const[])
{
	++kernel_entries;
	if (!claim_guest_ram)
		return kernel_left_memory;
	const int memory = darwin_uml_open("uml:anonymous-physical-memory",
		O_RDWR, 0);
	if (memory < 0)
		return kernel_without_memory;
	(void)::close(memory);
	return kernel_booted;
}

using Emulator =
	std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)>;

Emulator make_emulator()
{
	const uml_emulator_config config{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = "/tmp/uml-sequential-runs.img",
		.native_kernel_entry = kernel,
	};
	uml_emulator *created = nullptr;
	require(uml_emulator_create(&config, &created) == UML_EMULATOR_OK &&
			created != nullptr,
		"could not create an emulator handle");
	return {created, uml_emulator_destroy};
}

int run(uml_emulator *emulator)
{
	const char *arguments[]{"uml-sequential-runs"};
	return uml_emulator_run(emulator, 1, arguments, nullptr);
}

void require_first_guest(uml_emulator *emulator)
{
	const int entries = kernel_entries;
	require(run(emulator) == kernel_booted,
		"the first guest in the process could not open guest RAM");
	require(kernel_entries == entries + 1,
		"the first guest did not enter its kernel once");
}

void require_refused(uml_emulator *emulator)
{
	const int entries = kernel_entries;
	require(run(emulator) == -UML_EMULATOR_UNSUPPORTED,
		"a run after the process's guest did not fail as unsupported");
	require(kernel_entries == entries,
		"a run after the process's guest entered its kernel");
}

void second_run_on_the_same_handle()
{
	auto emulator = make_emulator();
	require_first_guest(emulator.get());
	require_refused(emulator.get());
	require_refused(emulator.get());
}

void second_run_on_a_fresh_handle()
{
	{
		auto first = make_emulator();
		require_first_guest(first.get());
	}
	auto second = make_emulator();
	require_refused(second.get());
}

void run_that_left_guest_ram()
{
	auto emulator = make_emulator();
	claim_guest_ram = false;
	require(run(emulator.get()) == kernel_left_memory,
		"a kernel that left guest RAM did not return its status");
	claim_guest_ram = true;
	require_first_guest(emulator.get());
	require_refused(make_emulator().get());
}

} // namespace

int main(int argc, char **argv)
{
	try {
		const std::string_view mode = argc == 2 ? argv[1] : "";
		if (mode == "same-handle")
			second_run_on_the_same_handle();
		else if (mode == "fresh-handle")
			second_run_on_a_fresh_handle();
		else if (mode == "unclaimed-first-run")
			run_that_left_guest_ram();
		else
			throw std::runtime_error{
				"usage: uml_runtime_sequential_run_tests "
				"same-handle|fresh-handle|unclaimed-first-run"};
	} catch (const std::exception &error) {
		std::fprintf(stderr, "%s\n", error.what());
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
