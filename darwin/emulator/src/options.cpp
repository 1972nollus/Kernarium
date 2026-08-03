/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "options.hpp"

#include <algorithm>
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

[[nodiscard]] bool informational(
	const std::vector<std::string> &arguments)
{
	return has_argument(arguments, "--help") ||
		has_argument(arguments, "--version") ||
		has_argument(arguments, "--showconfig");
}

[[nodiscard]] bool parse_execution_engine(
	std::string_view value, darwin_uml_execution_engine &engine)
{
	if (value == "auto")
		engine = DARWIN_UML_EXECUTION_AUTO;
	else if (value == "jit")
		engine = DARWIN_UML_EXECUTION_JIT;
	else if (value == "gadgets")
		engine = DARWIN_UML_EXECUTION_GADGETS;
	else if (value == "interpreter")
		engine = DARWIN_UML_EXECUTION_INTERPRETER;
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
	if (argc < 0 || (argc != 0 && argv == nullptr)) {
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
	// The project rootfs is a frozen GPT image.  A writable `ubd0=` mapping
	// would bypass OverlayFS and can silently damage the shared fixture.
	if (has_prefix(prepared.arguments, "ubd0=")) {
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
