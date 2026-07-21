// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/backend.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <mach/arm/thread_status.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>

extern "C" {
extern const unsigned char uml_elf_start[]
	__asm("section$start$__DATA$__uml_elf");
extern const unsigned char uml_elf_end[]
	__asm("section$end$__DATA$__uml_elf");
}

namespace {

void host_fault(int signal, siginfo_t *info, void *context_pointer)
{
	auto &context = *static_cast<ucontext_t *>(context_pointer);
	const auto pc = static_cast<std::uintptr_t>(
		__darwin_arm_thread_state64_get_pc(context.uc_mcontext->__ss));
	dprintf(STDERR_FILENO,
		"Darwin UML host signal %d at pc=%p address=%p\n", signal,
		reinterpret_cast<void *>(pc), info->si_addr);
	_exit(128 + signal);
}

void install_host_faults()
{
	constexpr std::array signals{SIGABRT, SIGBUS, SIGSEGV, SIGILL};
	struct sigaction action {};
	action.sa_sigaction = host_fault;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	for (const int signal : signals)
		if (sigaction(signal, &action, nullptr) < 0)
			std::abort();
}

bool has_argument(int argc, char *const argv[], std::string_view argument)
{
	for (int index = 1; index < argc; ++index)
		if (argv[index] != nullptr && argument == argv[index])
			return true;
	return false;
}

bool has_prefix(int argc, char *const argv[], std::string_view prefix)
{
	for (int index = 1; index < argc; ++index)
		if (argv[index] != nullptr &&
		    std::string_view{argv[index]}.starts_with(prefix))
			return true;
	return false;
}

bool informational(int argc, char *const argv[])
{
	return has_argument(argc, argv, "--help") ||
		has_argument(argc, argv, "--version") ||
		has_argument(argc, argv, "--showconfig");
}

bool host_has_lse_atomics()
{
	int supported = 0;
	std::size_t size = sizeof(supported);
	return sysctlbyname("hw.optional.arm.FEAT_LSE", &supported, &size,
			    nullptr, 0) == 0 &&
		size == sizeof(supported) && supported != 0;
}

bool usable_rootfs(const std::filesystem::path &path)
{
	return access(path.c_str(), R_OK | W_OK) == 0;
}

std::optional<std::filesystem::path> canonical_rootfs(
	const std::filesystem::path &candidate)
{
	std::error_code error;
	auto path = std::filesystem::canonical(candidate, error);
	if (!error && usable_rootfs(path))
		return path;
	return std::nullopt;
}

std::optional<std::filesystem::path> locate_rootfs()
{
	if (const char *configured = std::getenv("UML_ROOTFS");
	    configured != nullptr && configured[0] != '\0') {
		auto path = canonical_rootfs(configured);
		if (!path)
			std::fprintf(stderr,
				"UML_ROOTFS is not a writable ext2 image: %s\n",
				configured);
		return path;
	}

	std::array<char, 4096> executable{};
	std::uint32_t length = executable.size();
	if (_NSGetExecutablePath(executable.data(), &length) == 0) {
		std::error_code error;
		auto parent = std::filesystem::canonical(executable.data(), error)
			.parent_path();
		if (!error)
			if (auto path = canonical_rootfs(parent / "rootfs.ext2"))
				return path;
	}
	if (auto path = canonical_rootfs("rootfs.ext2"))
		return path;
	std::fputs("rootfs.ext2 was not found beside the executable or in the "
		"current directory\n", stderr);
	return std::nullopt;
}

} // namespace

int main(int argc, char *argv[], char *envp[])
{
	install_host_faults();
	if (!informational(argc, argv) && !host_has_lse_atomics()) {
		std::fputs("This Darwin UML kernel requires Armv8.1 atomic "
			   "instructions on the host\n", stderr);
		return 1;
	}
	std::string rootfs;
	if (!informational(argc, argv) && !has_prefix(argc, argv, "ubd0")) {
		auto located = locate_rootfs();
		if (!located)
			return 1;
		rootfs = located->string();
	}

	const uml_backend_config config{
		.abi_version = UML_BACKEND_ABI_VERSION,
		.struct_size = sizeof(uml_backend_config),
		.rootfs_path = rootfs.empty() ? nullptr : rootfs.c_str(),
		.kernel_image = uml_elf_start,
		.kernel_image_size = static_cast<std::size_t>(
			uml_elf_end - uml_elf_start),
		.native_kernel_entry = nullptr,
		.network = nullptr,
	};
	uml_backend *created = nullptr;
	const int create_result = uml_backend_create(&config, &created);
	if (create_result != UML_BACKEND_OK) {
		std::fprintf(stderr, "Unable to create UML backend: %d\n",
			create_result);
		return 1;
	}
	const std::unique_ptr<uml_backend, decltype(&uml_backend_destroy)> backend{
		created, uml_backend_destroy};
	std::vector<const char *> arguments;
	arguments.reserve(static_cast<std::size_t>(argc));
	for (int index = 0; index < argc; ++index)
		arguments.push_back(argv[index]);
	std::vector<const char *> environment;
	for (auto entry = envp; entry != nullptr && *entry != nullptr; ++entry)
		environment.push_back(*entry);
	environment.push_back(nullptr);
	const int result = uml_backend_run(backend.get(), argc, arguments.data(),
		environment.data());
	/* The embedded ELF kernel installs process-wide Linux signal handlers.
	 * Once it halts, avoid running unrelated C++ static teardown through that
	 * signal environment.  Native app-linked kernels can use normal frontend
	 * lifecycle management.
	 */
	std::_Exit(result);
}
