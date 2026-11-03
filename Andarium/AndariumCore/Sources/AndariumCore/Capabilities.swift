// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the runtime and storage behind the screens can do. A screen shows a
/// control only when the capability behind it exists, so the application
/// never offers what it cannot do. The Simulator preview has all of them; the
/// device application has the ones its runtime provides.
public struct Capabilities: Equatable, Sendable {
    /// The app can move an existing guest to a newer system.
    public var systemUpdate: Bool
    /// A computer can reach the guest with ADB over the network.
    public var adbOverNetwork: Bool
    /// An app picked in Files (an .apk) can be installed into the guest.
    public var apkInstall: Bool
    /// The guest's bug report can be made and shared.
    public var bugReport: Bool
    /// The session can show the guest's own statistics.
    public var statistics: Bool
    /// Resolution and Follow Window Size can change while the guest runs;
    /// otherwise they apply at the next start and the session leaves them
    /// out.
    public var liveDisplayOptions: Bool

    public init(
        systemUpdate: Bool, adbOverNetwork: Bool, apkInstall: Bool, bugReport: Bool,
        statistics: Bool, liveDisplayOptions: Bool
    ) {
        self.systemUpdate = systemUpdate
        self.adbOverNetwork = adbOverNetwork
        self.apkInstall = apkInstall
        self.bugReport = bugReport
        self.statistics = statistics
        self.liveDisplayOptions = liveDisplayOptions
    }

    /// Everything the mock runtime pretends to do.
    public static let preview = Capabilities(
        systemUpdate: true, adbOverNetwork: true, apkInstall: true, bugReport: true, statistics: true,
        liveDisplayOptions: true
    )

    /// The device application: a guest moves to a newer system with its
    /// data (`StartPlan`), and the session offers an app install, ADB over
    /// the network, a bug report and the options, all through the runtime's
    /// adb channel.
    public static let device = Capabilities(
        systemUpdate: true,
        adbOverNetwork: true,
        apkInstall: true,
        bugReport: true,
        statistics: false, liveDisplayOptions: false
    )
}
