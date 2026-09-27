// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A process can boot a guest kernel once. The host accepts one guest-RAM token
 * per process and never releases it, and it names the management console once
 * per process while the guest never removes the socket it binds under that
 * name. These checks pin that behaviour for a second run on the same handle
 * and on a fresh handle. A change that lifts the limit fails here; turn the
 * failing check into a regression test for sequential runs.
 *
 * Each mode runs in its own process because the registration outlives every
 * handle.
 */
#include "uml/emulator.h"
#include "uml_darwin_host.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

constexpr int kernel_booted = 41;
constexpr int kernel_without_memory = 42;

struct RunRecord final {
	std::string umid;
	int memory_error{};
};

RunRecord last_run;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error{message};
}

void require_known_limit(bool still_limited, const char *message)
{
	if (!still_limited)
		throw std::runtime_error{std::string{"known limit lifted: "} +
			message + "; make this a sequential-run regression check"};
}

/* Stands in for the start of a UML boot: the kernel takes its console name
 * from the command line, and setup_physmem() opens guest RAM. */
int kernel(int argc, char *const argv[], char *const[])
{
	last_run = {};
	for (int index = 0; index < argc; ++index) {
		const std::string_view argument{argv[index]};
		if (argument.starts_with("umid="))
			last_run.umid = argument.substr(5);
	}
	const int memory = darwin_uml_open("uml:anonymous-physical-memory",
		O_RDWR, 0);
	if (memory < 0) {
		last_run.memory_error = errno;
		return kernel_without_memory;
	}
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

void check_second_run(int result, const std::string &first_umid)
{
	require(result == kernel_booted || result == kernel_without_memory,
		"the second run did not reach the kernel entry");
	require_known_limit(result == kernel_without_memory &&
			last_run.memory_error == EBUSY,
		"a second run opened guest RAM");
	require(!first_umid.empty() && !last_run.umid.empty(),
		"a run was not given a management-console name");
	require_known_limit(last_run.umid == first_umid,
		"a second run got its own management-console name");
}

void second_run_on_the_same_handle()
{
	auto emulator = make_emulator();
	require(run(emulator.get()) == kernel_booted,
		"the first run in the process could not open guest RAM");
	const auto first_umid = last_run.umid;
	check_second_run(run(emulator.get()), first_umid);
}

void second_run_on_a_fresh_handle()
{
	std::string first_umid;
	{
		auto first = make_emulator();
		require(run(first.get()) == kernel_booted,
			"the first run in the process could not open guest RAM");
		first_umid = last_run.umid;
	}
	auto second = make_emulator();
	check_second_run(run(second.get()), first_umid);
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
		else
			throw std::runtime_error{
				"usage: uml_runtime_sequential_run_tests "
				"same-handle|fresh-handle"};
	} catch (const std::exception &error) {
		std::fprintf(stderr, "%s\n", error.what());
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
