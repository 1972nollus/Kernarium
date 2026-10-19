// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
@testable import Kernarium

/// iOS can end a suspended application without running any of its code, and
/// the guest it hosted goes with it. The application records the background
/// edge it can see so that the next launch can say what happened.
@MainActor
struct BackgroundSessionTests {
    private func scratchMarker() throws -> BackgroundMarker {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("background-session-\(UUID().uuidString)")
        try FileManager.default.createDirectory(
            at: directory, withIntermediateDirectories: true
        )
        return BackgroundMarker(
            url: directory.appendingPathComponent("marker.json")
        )
    }

    @Test
    func aMarkerLeftInTheBackgroundIsReportedOnceAtTheNextLaunch() throws {
        let marker = try scratchMarker()
        let entered = Date(timeIntervalSince1970: 1_800_000_000)
        #expect(marker.arm(at: entered))

        let relaunched = BackgroundSession(marker: marker)
        #expect(relaunched.restart?.enteredBackgroundAt == entered)
        #expect(relaunched.restart?.title == "Guest Restarted")
        #expect(relaunched.restart?.message.contains("closed") == true)

        // Reported once: the launch after it starts clean.
        #expect(BackgroundSession(marker: marker).restart == nil)
    }

    @Test
    func returningToTheForegroundMeansThereIsNothingToReport() throws {
        let marker = try scratchMarker()
        let session = BackgroundSession(marker: marker)
        session.didEnterBackground()
        #expect(FileManager.default.fileExists(atPath: marker.url.path))
        session.willEnterForeground()
        #expect(!FileManager.default.fileExists(atPath: marker.url.path))
        #expect(BackgroundSession(marker: marker).restart == nil)
    }

    @Test
    func aProcessThatExitsOnItsOwnAccountLeavesNoMarker() throws {
        let marker = try scratchMarker()
        let session = BackgroundSession(marker: marker)
        session.didEnterBackground()
        session.willTerminate()
        #expect(BackgroundSession(marker: marker).restart == nil)
    }

    @Test
    func anUnreadableMarkerStillCountsAndNoticeCanBeDismissed() throws {
        let marker = try scratchMarker()
        try Data("not json".utf8).write(to: marker.url)
        let session = BackgroundSession(marker: marker)
        #expect(session.restart != nil)
        session.dismissRestartNotice()
        #expect(session.restart == nil)
        #expect(!FileManager.default.fileExists(atPath: marker.url.path))
    }

    @Test
    func aLaunchWithNoMarkerReportsNothing() throws {
        let marker = try scratchMarker()
        #expect(BackgroundSession(marker: marker).restart == nil)
    }
}
