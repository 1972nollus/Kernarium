/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "options.hpp"

#include <algorithm>
#include <limits>
#include <string_view>

namespace uml::emulator {
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

[[nodiscard]] bool has_writable_primary_block_device(
	const std::vector<std::string> &arguments)
{
	for (const auto &argument : arguments) {
		constexpr std::string_view device{"ubd0"};
		if (!argument.starts_with(device))
			continue;
		const auto separator = argument.find('=', device.size());
		if (separator == std::string::npos ||
		    argument.find('r', device.size()) >= separator)
			return true;
	}
	return false;
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
		prepared.arguments.emplace_back(argument);
	}
	if (prepared.arguments.empty())
		prepared.arguments.emplace_back("uml-darwin");
	if (informational(prepared.arguments))
		return prepared;
	// The project rootfs is a frozen GPT image. Every valid UML block-device
	// flag combination is writable unless it contains `r`; accepting only the
	// plain `ubd0=` spelling here would leave `ubd0s=`, `ubd0c=`, and the other
	// flag variants able to bypass OverlayFS and damage the shared fixture.
	if (has_writable_primary_block_device(prepared.arguments)) {
		prepared.valid = false;
		return prepared;
	}
	if (!has_prefix(prepared.arguments, "ubd0")) {
		if (rootfs.empty()) {
			prepared.valid = false;
			return prepared;
		}
		prepared.arguments.emplace_back("ubd0r=" + std::string(rootfs));
	}
	if (!has_prefix(prepared.arguments, "root="))
		prepared.arguments.emplace_back("root=/dev/ubda1");
	if (!has_prefix(prepared.arguments, "init="))
		prepared.arguments.emplace_back("init=/sbin/uml-overlay-init");
	if (!has_prefix(prepared.arguments, "mem="))
		prepared.arguments.emplace_back("mem=256M");
	if (default_user_network && !has_prefix(prepared.arguments, "ip="))
		prepared.arguments.emplace_back("ip=dhcp");
	if (has_argument(prepared.arguments, "rw")) {
		prepared.valid = false;
		return prepared;
	}
	if (!has_argument(prepared.arguments, "ro"))
		prepared.arguments.emplace_back("ro");
	return prepared;
}

} // namespace uml::emulator
