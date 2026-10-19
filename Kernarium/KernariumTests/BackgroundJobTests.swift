// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
@testable import Kernarium

/// The progress a continued task reports is the time it has used, and the
/// system must never see it complete before the time is up.
struct BackgroundJobTests {
    private let start = Date(timeIntervalSinceReferenceDate: 1_000_000)

    private func timeline(minutes: Int) -> BackgroundJobTimeline {
        BackgroundJobTimeline(started: start, limit: TimeInterval(minutes * 60))
    }

    @Test
    func progressFollowsTheTimeUsedAndNeverCompletesEarly() {
        let job = timeline(minutes: 30)
        #expect(job.totalUnits == 1800)
        #expect(job.completedUnits(at: start) == 0)
        #expect(job.completedUnits(at: start.addingTimeInterval(600)) == 600)
        // Held below the total until the time is up.
        #expect(job.completedUnits(at: start.addingTimeInterval(1799.5)) == 1799)
        #expect(!job.isOver(at: start.addingTimeInterval(1799.5)))
        #expect(job.isOver(at: start.addingTimeInterval(1800)))
        #expect(job.completedUnits(at: start.addingTimeInterval(1800)) == 1800)
        #expect(job.completedUnits(at: start.addingTimeInterval(9000)) == 1800)
    }

    @Test
    func aClockBeforeTheStartCountsAsNothingUsed() {
        let job = timeline(minutes: 15)
        #expect(job.elapsed(at: start.addingTimeInterval(-30)) == 0)
        #expect(job.remaining(at: start.addingTimeInterval(-30)) == 900)
    }

    @Test
    func theCardSaysHowMuchTimeIsLeftInWholeMinutes() {
        let job = timeline(minutes: 30)
        #expect(job.subtitle(at: start) == "30 minutes left")
        #expect(job.subtitle(at: start.addingTimeInterval(61)) == "29 minutes left")
        #expect(job.subtitle(at: start.addingTimeInterval(1750)) == "1 minute left")
        #expect(job.subtitle(at: start.addingTimeInterval(1800)) == "Finishing")
    }
}
