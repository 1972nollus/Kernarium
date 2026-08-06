// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::runtime {

/* Host-side policy only. Linux selects no execution engine through the fixed
 * C bridge; public handles and internal tests select it through Context. */
enum class ExecutionEngine : std::uint8_t {
	automatic = 0,
	gadgets = 1,
	jit = 2,
	interpreter = 3,
};

[[nodiscard]] bool execution_engine_available(
	ExecutionEngine engine) noexcept;

} // namespace uml::runtime
