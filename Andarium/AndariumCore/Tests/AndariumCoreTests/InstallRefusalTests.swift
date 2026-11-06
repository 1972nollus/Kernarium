// SPDX-License-Identifier: GPL-2.0-or-later

import Testing

@testable import AndariumCore

struct InstallRefusalTests {
    @Test func aSingleApkIsTaken() {
        for name in ["Signal-7.21.4.apk", "A.APK", "my app.Apk"] {
            #expect(InstallRefusal(fileName: name) == nil, "\(name)")
        }
    }

    @Test func aFileThatIsNotAnAppIsTurnedAway() {
        for name in ["Trip photos.zip", "notes.txt", "apk", "app.apk.zip", ""] {
            #expect(InstallRefusal(fileName: name) == .notApk, "\(name)")
        }
    }

    @Test func anAppInSeveralFilesIsTurnedAwayWithItsOwnWords() {
        for name in ["app.apks", "Trip Planner.XAPK", "app.apkm"] {
            #expect(InstallRefusal(fileName: name) == .bundle, "\(name)")
        }
        #expect(InstallRefusal.notApk.message == "This isn\u{2019}t an app. Choose a file that ends in .apk.")
        #expect(InstallRefusal.bundle.message
            == "Apps that come in several files can\u{2019}t be installed yet. Choose a single .apk file.")
    }
}
