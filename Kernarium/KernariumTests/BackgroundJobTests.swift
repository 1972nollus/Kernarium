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

/// An automation launch can ask for the job to start with its guest. Nothing
/// else does: the release app starts a job only on the user's tap.
@MainActor
struct BackgroundJobAtStartTests {
    private let timeline = BackgroundJobTimeline(
        started: Date(timeIntervalSinceReferenceDate: 1_000_000),
        limit: 900
    )

    @Test
    func aGuestThatKeepsRunningStartsItsJobWhenAutomationAsks() {
        #expect(BackgroundJob.startsWithGuest(
            asked: true, mode: .keepRunning, state: .idle
        ))
    }

    @Test
    func nothingStartsAJobWithoutTheAsk() {
        for mode in [BackgroundMode.off, .pictureInPicture, .keepRunning] {
            #expect(!BackgroundJob.startsWithGuest(
                asked: false, mode: mode, state: .idle
            ))
        }
    }

    @Test
    func theAskStartsNothingForAGuestThatChoseAnotherMode() {
        for mode in [BackgroundMode.off, .pictureInPicture] {
            #expect(!BackgroundJob.startsWithGuest(
                asked: true, mode: mode, state: .idle
            ))
        }
    }

    @Test
    func theAskStartsNoSecondJob() {
        for state in [BackgroundJob.State.submitted, .running(timeline)] {
            #expect(!BackgroundJob.startsWithGuest(
                asked: true, mode: .keepRunning, state: state
            ))
        }
    }

    /// The setting's name is in the automation runtimes alone, so a launch
    /// that does not set it, or a runtime that has no such name, is not asked.
    @Test
    func aLaunchThatDoesNotAskIsNeverAsked() {
        #expect(
            ProcessInfo.processInfo.environment[
                "KERNARIUM_BACKGROUND_JOB_AT_START"
            ] == nil,
            "this test needs a launch that did not ask"
        )
        #expect(kernarium_runtime_loader_automation_job_at_start() == 0)
    }
}
