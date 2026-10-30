// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstring>

namespace kernarium::automation {

/* The environment variable that asks an automation launch to start the
 * background job as soon as its guest runs, the action the job button takes.
 * Only the automation runtimes include this header, so only they contain the
 * name; the packaging script refuses a release runtime that does. */
inline constexpr char jobAtStartVariable[] = "KERNARIUM_BACKGROUND_JOB_AT_START";

enum class JobAtStart { off, on, invalid };

/* "1" asks for the job, "0" and an unset variable do not, and anything else is
 * a mistake of the driver that the runtime reports and does not act on. */
[[nodiscard]] inline JobAtStart parseJobAtStart(const char* value) noexcept
{
    if (value == nullptr || std::strcmp(value, "0") == 0)
        return JobAtStart::off;
    return std::strcmp(value, "1") == 0 ? JobAtStart::on : JobAtStart::invalid;
}

} // namespace kernarium::automation
