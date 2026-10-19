// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::platform {

/* Whether this process is an iOS application running on macOS.
 *
 * An iOS build in Designed for iPad mode keeps the iOS SDK, the iOS sandbox,
 * and the iOS code-signing rules, but it runs inside a macOS process and
 * therefore inherits the macOS address-space layout rather than a device's.
 * Anything that places host mappings at a fixed address has to ask.
 */
[[nodiscard]] bool running_ios_app_on_macos() noexcept;

/* The memory footprint the host lets this process reach before it ends it,
 * zero when it sets no such limit: iOS sets one per application (jetsam), and
 * macOS sets none for an ordinary process. One task_info call. */
[[nodiscard]] std::uint64_t process_memory_limit_bytes() noexcept;

} // namespace uml::platform
