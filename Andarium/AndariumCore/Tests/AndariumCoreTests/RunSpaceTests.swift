// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

struct RunSpaceWatchTests {
    private let gib = Limits.bytesPerGiB
    private let mib = Limits.bytesPerMiB

    @Test func theThresholdsFormALadderBelowAStart() {
        #expect(Limits.pauseBelowBytes == gib)
        #expect(Limits.resumeAtBytes == Limits.startReserveBytes)
        // A guest is paused below what is needed to start it, and resumed at
        // that, so a resume is never followed straight away by a pause.
        #expect(Limits.pauseBelowBytes < Limits.resumeAtBytes)
        #expect(Limits.resumeAtBytes <= Limits.firstStartReserveBytes)
    }

    @Test func aGuestRunsAtAndAboveTheThreshold() {
        var watch = RunSpaceWatch()
        let roomy = watch.sample(freeBytes: 40 * gib)
        let atTheThreshold = watch.sample(freeBytes: gib)
        #expect(roomy == .keepRunning)
        #expect(atTheThreshold == .keepRunning)
        #expect(!watch.isPaused)
    }

    @Test func aGuestIsPausedBelowTheThreshold() {
        var watch = RunSpaceWatch()
        let verdict = watch.sample(freeBytes: gib - 1)
        #expect(verdict == .pause)
        #expect(watch.isPaused)
        var empty = RunSpaceWatch()
        let emptyVerdict = empty.sample(freeBytes: 0)
        #expect(emptyVerdict == .pause)
    }

    @Test func aPausedGuestResumesOnlyAtTheResumeSpace() {
        var watch = RunSpaceWatch()
        _ = watch.sample(freeBytes: 900 * mib)
        // The pause threshold is not enough to resume: the same space would
        // pause the guest again at the next write.
        let atThePause = watch.resume(freeBytes: gib)
        let justShort = watch.resume(freeBytes: Limits.resumeAtBytes - 1)
        #expect(!atThePause && !justShort)
        #expect(watch.isPaused)
        let enough = watch.resume(freeBytes: Limits.resumeAtBytes)
        #expect(enough)
        #expect(!watch.isPaused)
    }

    @Test func aGuestThatIsNotPausedHasNothingToResume() {
        var watch = RunSpaceWatch()
        let resumed = watch.resume(freeBytes: 40 * gib)
        #expect(!resumed)
    }

    @Test func resettingTheWatchEndsAPause() {
        var watch = RunSpaceWatch()
        _ = watch.sample(freeBytes: 0)
        watch.reset()
        #expect(!watch.isPaused)
        let verdict = watch.sample(freeBytes: 2 * gib)
        #expect(verdict == .keepRunning)
    }

    @Test func theNextCheckFollowsTheSpaceThatCouldBeWrittenBeforeThePause() {
        // 128 MiB a second over the margin to the pause.
        #expect(RunSpaceWatch.nextCheck(freeBytes: gib + 512 * mib) == .seconds(4))
        #expect(RunSpaceWatch.nextCheck(freeBytes: gib + 1024 * mib) == .seconds(8))
    }

    @Test func theNextCheckIsBoundedBothWays() {
        #expect(RunSpaceWatch.nextCheck(freeBytes: gib) == RunSpaceWatch.shortestInterval)
        #expect(RunSpaceWatch.nextCheck(freeBytes: gib + mib) == RunSpaceWatch.shortestInterval)
        #expect(RunSpaceWatch.nextCheck(freeBytes: 0) == RunSpaceWatch.shortestInterval)
        #expect(RunSpaceWatch.nextCheck(freeBytes: 100 * gib) == RunSpaceWatch.longestInterval)
        #expect(RunSpaceWatch.nextCheck(freeBytes: .max) == RunSpaceWatch.longestInterval)
    }

    @Test func theNextCheckNeverLetsTheGuestWriteIntoThePauseMargin() {
        // At every free size the guest writing flat out for the interval
        // stays above zero: the interval is the time to write the margin, or
        // the shortest, whose writes the threshold leaves room for.
        var free = Limits.pauseBelowBytes
        while free < 40 * gib {
            let interval = RunSpaceWatch.nextCheck(freeBytes: free)
            let seconds = Double(interval.components.seconds)
            let written = UInt64(seconds) * RunSpaceWatch.assumedWriteRate
            if interval > RunSpaceWatch.shortestInterval, interval < RunSpaceWatch.longestInterval {
                #expect(free - written >= Limits.pauseBelowBytes - RunSpaceWatch.assumedWriteRate)
            }
            free += 97 * mib
        }
        let shortest = UInt64(RunSpaceWatch.shortestInterval.components.seconds) * RunSpaceWatch.assumedWriteRate
        #expect(shortest < Limits.pauseBelowBytes / 2)
    }
}
