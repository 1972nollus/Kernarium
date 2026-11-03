// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "KernariumRuntime.h"

#include <uml/emulator.h>

#include <cerrno>
#include <cstdint>
#include <optional>

namespace kernarium::statistics {

/* What the application's readings are made of, and when asking the guest for
 * its memory is allowed. These are the runtime's rules alone, apart from the
 * emulator, the GPU service and the locks they sit behind, so they run on any
 * host. Nothing here keeps state: the runtime has no sampler, timer or counter
 * of its own, and a reading is whatever the sources say when it is taken. */

/* The frames a display counted, as the GPU service reports them. */
struct DisplayFigures final {
    std::uint64_t presentedFrames {};
    std::uint64_t offeredFrames {};
};

/* A display's primary scanout, as the runtime's guest_display_active reports
 * it: 1 on, 0 off, -1 when there is no guest or its GPU has not started. */
[[nodiscard]] constexpr std::int32_t displayActive(
    std::optional<bool> scanout) noexcept
{
    return scanout.has_value() ? (*scanout ? 1 : 0) : -1;
}

/* One reading. `guest` is the emulator's, `display` the GPU service's (none
 * when no service is registered, which reads as no frames), `bootIndex` the
 * restart count of the launch's guest, and `timestampNs` the steady clock when
 * the guest's CPU time was read. */
[[nodiscard]] constexpr kernarium_runtime_statistics reading(
    const uml_emulator_statistics& guest,
    std::optional<DisplayFigures> display,
    std::optional<bool> scanout,
    std::uint32_t bootIndex,
    std::uint64_t timestampNs) noexcept
{
    kernarium_runtime_statistics result {};
    result.struct_size = sizeof(result);
    result.processor_count = guest.guest_cpu_count;
    result.boot_index = bootIndex;
    result.display_active = displayActive(scanout);
    result.timestamp_ns = timestampNs;
    result.processor_time_ns = guest.guest_cpu_time_ns;
    result.presented_frames = display ? display->presentedFrames : 0U;
    result.offered_frames = display ? display->offeredFrames : 0U;
    result.memory_bytes = guest.guest_ram_bytes;
    result.memory_held_bytes = guest.guest_ram_held_bytes;
    return result;
}

/* The runtime's answer for an emulator statistics call: a handle that does
 * not run has no guest. */
[[nodiscard]] constexpr int statisticsResult(int emulator) noexcept
{
    if (emulator == UML_EMULATOR_OK)
        return 0;
    if (emulator == -UML_EMULATOR_UNSUPPORTED)
        return -ENODEV;
    if (emulator == -UML_EMULATOR_INVALID_ARGUMENT)
        return -EINVAL;
    return -EIO;
}

/* Whether the guest's memory may be asked for now, and the answer that says
 * why not. A request to the management console ends a suspend, so it is not
 * sent while the application has quiesced the guest, while a lifecycle call
 * (a quiesce, resume, sync or trimming change, which hold the lifecycle lock
 * for seconds) is in progress, or while the guest's display is known to be
 * off: that is when an Android guest suspends itself. A display whose state is
 * unknown (no GPU yet) does not forbid it. -EAGAIN means ask again later,
 * -ENODEV that no guest runs. */
[[nodiscard]] constexpr int memoryRefusal(
    bool lifecycleFree,
    bool guestRuns,
    bool quiesced,
    std::optional<bool> scanout) noexcept
{
    if (!lifecycleFree)
        return -EAGAIN;
    if (!guestRuns)
        return -ENODEV;
    if (quiesced)
        return -EAGAIN;
    if (scanout.has_value() && !*scanout)
        return -EAGAIN;
    return 0;
}

/* The runtime's answer for an emulator memory call: a kernel without the
 * request is unsupported, a guest that did not answer in time timed out. */
[[nodiscard]] constexpr int guestMemoryResult(int emulator) noexcept
{
    if (emulator == UML_EMULATOR_OK)
        return 0;
    if (emulator == -UML_EMULATOR_UNSUPPORTED)
        return -ENOTSUP;
    if (emulator == -UML_EMULATOR_RUNTIME_ERROR)
        return -ETIMEDOUT;
    if (emulator == -UML_EMULATOR_INVALID_ARGUMENT)
        return -EINVAL;
    return -EIO;
}

}  // namespace kernarium::statistics
