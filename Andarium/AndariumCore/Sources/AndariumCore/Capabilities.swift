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
    /// An APK picked in Files can be installed into the guest.
    public var apkInstall: Bool
    /// The session can show the guest's own statistics.
    public var statistics: Bool
    /// The number of installed apps can be read.
    public var appCount: Bool

    public init(
        systemUpdate: Bool, adbOverNetwork: Bool, apkInstall: Bool,
        statistics: Bool, appCount: Bool
    ) {
        self.systemUpdate = systemUpdate
        self.adbOverNetwork = adbOverNetwork
        self.apkInstall = apkInstall
        self.statistics = statistics
        self.appCount = appCount
    }

    /// Everything the mock runtime pretends to do.
    public static let preview = Capabilities(
        systemUpdate: true, adbOverNetwork: true, apkInstall: true, statistics: true, appCount: true
    )

    /// The first release: a guest keeps the system it was created with, and
    /// the session offers Android's own screen, keys and the options.
    public static let firstRelease = Capabilities(
        systemUpdate: false, adbOverNetwork: false, apkInstall: false, statistics: false, appCount: false
    )
}
