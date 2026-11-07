// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

struct RunSpaceTests {
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
        #expect(RunSpace.verdict(freeBytes: 40 * gib) == .keepRunning)
        #expect(RunSpace.verdict(freeBytes: gib) == .keepRunning)
    }

    @Test func aGuestIsPausedBelowTheThreshold() {
        #expect(RunSpace.verdict(freeBytes: gib - 1) == .pause)
        #expect(RunSpace.verdict(freeBytes: 0) == .pause)
    }

    @Test func aPausedGuestResumesOnlyAtTheResumeSpace() {
        // The pause threshold is not enough to resume: the same space would
        // pause the guest again at the next write.
        #expect(!RunSpace.allowsResume(freeBytes: 900 * mib))
        #expect(!RunSpace.allowsResume(freeBytes: gib))
        #expect(!RunSpace.allowsResume(freeBytes: Limits.resumeAtBytes - 1))
        #expect(RunSpace.allowsResume(freeBytes: Limits.resumeAtBytes))
        #expect(RunSpace.allowsResume(freeBytes: 40 * gib))
    }

    @Test func theNextCheckFollowsTheSpaceThatCouldBeWrittenBeforeThePause() {
        // 128 MiB a second over the margin to the pause.
        #expect(RunSpace.nextCheck(freeBytes: gib + 512 * mib) == .seconds(4))
        #expect(RunSpace.nextCheck(freeBytes: gib + 1024 * mib) == .seconds(8))
    }

    @Test func theNextCheckIsBoundedBothWays() {
        #expect(RunSpace.nextCheck(freeBytes: gib) == RunSpace.shortestInterval)
        #expect(RunSpace.nextCheck(freeBytes: gib + mib) == RunSpace.shortestInterval)
        #expect(RunSpace.nextCheck(freeBytes: 0) == RunSpace.shortestInterval)
        #expect(RunSpace.nextCheck(freeBytes: 100 * gib) == RunSpace.longestInterval)
        #expect(RunSpace.nextCheck(freeBytes: .max) == RunSpace.longestInterval)
    }

    @Test func theNextCheckNeverLetsTheGuestWriteIntoThePauseMargin() {
        // Between the two bounds the interval is the time to write the margin
        // to the pause, so the guest writing flat out reaches the pause at the
        // next reading and not past it. The shortest interval's writes are
        // what the threshold leaves room for: well under half of it.
        var free = Limits.pauseBelowBytes
        while free < 40 * gib {
            let interval = RunSpace.nextCheck(freeBytes: free)
            if interval > RunSpace.shortestInterval, interval < RunSpace.longestInterval {
                let seconds = Double(interval.components.seconds)
                let written = UInt64(seconds) * RunSpace.assumedWriteRate
                #expect(written <= free - Limits.pauseBelowBytes)
            }
            free += 97 * mib
        }
        let shortest = UInt64(RunSpace.shortestInterval.components.seconds) * RunSpace.assumedWriteRate
        #expect(shortest < Limits.pauseBelowBytes / 2)
    }
}

struct SpacePauseTests {
    @Test func theFirstWordsGiveTheFreeSpaceAndWhatIsNeeded() {
        let pause = SpacePause(freeBytes: 820 * Limits.bytesPerMiB)
        #expect(pause.detail.contains("820 MiB"))
        #expect(pause.detail.contains("1.5 GiB"))
        #expect(!pause.detail.contains("Only"))
    }

    @Test func aResumeThatFoundTheSpaceShortSaysSo() {
        let pause = SpacePause(freeBytes: 1_200 * Limits.bytesPerMiB, stillShort: true)
        #expect(pause.detail.hasPrefix("Only "))
        #expect(pause.detail.contains("Free up more space"))
    }

    @Test func aShutdownNoLongerAsksForSpace() {
        let pause = SpacePause(freeBytes: 0, working: .shuttingDown)
        #expect(!pause.detail.contains("Free up"))
    }
}
