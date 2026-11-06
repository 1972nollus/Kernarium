// SPDX-License-Identifier: GPL-2.0-or-later

import Testing

@testable import AndariumCore

struct CapabilitiesTests {
    @Test func theDeviceOffersWhatTheAdbChannelDoes() {
        let device = Capabilities.device
        #expect(device.bugReport && device.apkInstall && device.adbOverNetwork)
        // The statistics have no runtime behind them yet.
        #expect(!device.statistics)
        #expect(!device.liveDisplayOptions)
    }

    @Test func thePreviewPretendsToDoEverything() {
        let preview = Capabilities.preview
        #expect(preview.bugReport && preview.apkInstall && preview.adbOverNetwork)
        #expect(preview.statistics && preview.liveDisplayOptions)
    }
}
