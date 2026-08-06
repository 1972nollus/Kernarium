// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace uml::gpu::testing {

enum class StartupFailure {
    none,
    afterTransportSocketPair,
    afterInputSocketPair,
    workerEntry,
};

void failNextStartup(StartupFailure failure) noexcept;
void startupCheckpoint(StartupFailure failure);

} // namespace uml::gpu::testing
