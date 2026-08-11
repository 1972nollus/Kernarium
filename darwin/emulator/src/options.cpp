/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "options.hpp"
#include <cstdlib>

#include <algorithm>
#include <limits>
#include <string_view>

namespace uml::emulator {

constexpr std::string_view jit_cache_budget_prefix{"--jit-cache-budget="};
namespace {

[[nodiscard]] bool has_argument(
	const std::vector<std::string> &arguments, std::string_view value)
{
	return std::ranges::any_of(arguments, [&](const auto &argument) {
		return argument == value;
	});
}

[[nodiscard]] bool has_prefix(
	const std::vector<std::string> &arguments, std::string_view prefix)
{
	return std::ranges::any_of(arguments, [&](const auto &argument) {
		return argument.starts_with(prefix);
	});
}

/* The guest's own memory size and CPU count, read from the kernel arguments
 * the host is about to pass it. Both keep their kernel meaning; the host only
 * reads them to decide how much it may spend on translated-code caches.
 */
[[nodiscard]] std::size_t guest_memory_bytes(
	const std::vector<std::string> &arguments)
{
	for (const auto &argument : arguments)
		if (argument.starts_with("mem="))
			return ::uml::runtime::parse_translation_cache_quantity(
				argument.substr(4).c_str());
	return 0;
}

[[nodiscard]] unsigned guest_cpu_count(
	const std::vector<std::string> &arguments)
{
	for (const auto &argument : arguments)
		if (argument.starts_with("ncpus=")) {
			const auto parsed = std::strtoul(
				argument.c_str() + 6, nullptr, 10);
			if (parsed != 0 && parsed <= 1024U)
				return static_cast<unsigned>(parsed);
		}
	return 1;
}

[[nodiscard]] bool informational(
	const std::vector<std::string> &arguments)
{
	return has_argument(arguments, "--help") ||
		has_argument(arguments, "--version") ||
		has_argument(arguments, "--showconfig");
}

[[nodiscard]] bool parse_execution_engine(
	std::string_view value, ::uml::runtime::ExecutionEngine &engine)
{
	if (value == "auto")
		engine = ::uml::runtime::ExecutionEngine::automatic;
	else if (value == "jit")
		engine = ::uml::runtime::ExecutionEngine::jit;
	else if (value == "gadgets")
		engine = ::uml::runtime::ExecutionEngine::gadgets;
	else if (value == "interpreter")
		engine = ::uml::runtime::ExecutionEngine::interpreter;
	else
		return false;
	return true;
}

} // namespace

EmulatorOptions prepare_emulator_options(
	int argc, const char *const argv[], std::string_view rootfs,
	bool default_user_network)
{
	EmulatorOptions prepared;
	/* Policy can append at most six kernel arguments. Keep the final argc
	 * representable by the fixed-width kernel entry ABI before allocating or
	 * inspecting a caller-supplied vector. */
	constexpr int maximum_appended_arguments = 6;
	if (argc < 0 ||
	    argc > std::numeric_limits<int>::max() - maximum_appended_arguments ||
	    (argc != 0 && argv == nullptr)) {
		prepared.valid = false;
		return prepared;
	}
	prepared.arguments.reserve(static_cast<std::size_t>(argc) + 6U);
	for (int index = 0; index < argc; ++index) {
		if (argv[index] == nullptr) {
			prepared.valid = false;
			return prepared;
		}
		const std::string_view argument{argv[index]};
		constexpr std::string_view prefix{"--execution-engine="};
		if (argument.starts_with(prefix)) {
			if (!parse_execution_engine(
					argument.substr(prefix.size()), prepared.engine)) {
				prepared.valid = false;
				return prepared;
			}
			continue;
		}
		if (argument == "--execution-engine") {
			if (++index >= argc || argv[index] == nullptr ||
				!parse_execution_engine(argv[index], prepared.engine)) {
				prepared.valid = false;
				return prepared;
			}
			continue;
		}
		if (argument.starts_with("--jit-gadget-fallback")) {
			prepared.valid = false;
			return prepared;
		}
		if (argument.starts_with(jit_cache_budget_prefix)) {
			/* Host-only, so it is consumed rather than handed to the
			 * guest kernel.
			 */
			const std::string quantity{argument.substr(
				jit_cache_budget_prefix.size())};
			prepared.jit_cache_budget =
				::uml::runtime::parse_translation_cache_quantity(
					quantity.c_str());
			if (prepared.jit_cache_budget == 0) {
				prepared.valid = false;
				return prepared;
			}
			continue;
		}
		prepared.arguments.emplace_back(argument);
	}
	if (prepared.arguments.empty())
		prepared.arguments.emplace_back("uml-darwin");
	if (informational(prepared.arguments))
		return prepared;
	// The unified rootfs is a raw EROFS image. The UBD `c` flag suppresses its
	// host-file lock so parallel guests can share the image; the rootfs sidecar
	// coordinates provisioning, while EROFS supplies the filesystem contract.
	if (!has_prefix(prepared.arguments, "ubd0")) {
		if (rootfs.empty()) {
			prepared.valid = false;
			return prepared;
		}
		prepared.arguments.emplace_back("ubd0c=" + std::string(rootfs));
	}
	if (!has_prefix(prepared.arguments, "root="))
		prepared.arguments.emplace_back("root=/dev/ubda");
	if (!has_prefix(prepared.arguments, "rootfstype="))
		prepared.arguments.emplace_back("rootfstype=erofs");
	if (!has_prefix(prepared.arguments, "init="))
		prepared.arguments.emplace_back("init=/bin/sh");
	if (!has_prefix(prepared.arguments, "mem="))
		prepared.arguments.emplace_back("mem=256M");
	if (default_user_network && !has_prefix(prepared.arguments, "ip="))
		prepared.arguments.emplace_back("ip=dhcp");
	prepared.guest_cpus = guest_cpu_count(prepared.arguments);
	if (prepared.jit_cache_budget == 0)
		prepared.jit_cache_budget =
			guest_memory_bytes(prepared.arguments) / 2U;
	return prepared;
}

} // namespace uml::emulator
