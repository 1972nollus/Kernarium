// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace uml::platform {

/* Matches the host query used by the maintained UML kernel when an explicit
 * ncpus= argument is absent.
 */
[[nodiscard]] unsigned online_processor_count() noexcept;

} // namespace uml::platform
