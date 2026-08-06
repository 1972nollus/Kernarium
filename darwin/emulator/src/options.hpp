/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "runtime/execution_engine.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace uml::emulator {

/*
 * Kernel command-line policy is shared by every composition root.  Keeping
 * it outside the public C ABI makes the policy testable without constructing
 * an emulator or linking a kernel image.
 */
struct EmulatorOptions final {
	std::vector<std::string> arguments;
	::uml::runtime::ExecutionEngine engine{
		::uml::runtime::ExecutionEngine::automatic};
	bool valid{true};
};

[[nodiscard]] EmulatorOptions prepare_emulator_options(
	int argc, const char *const argv[], std::string_view rootfs,
	bool default_user_network);

} // namespace uml::emulator
