// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace uml::gpu::testing {

inline void require(
    bool condition,
    const std::source_location location = std::source_location::current())
{
    if (condition)
        return;
    std::fprintf(
        stderr,
        "require failed at %s:%u\n",
        location.file_name(),
        location.line());
    std::abort();
}

} // namespace uml::gpu::testing
