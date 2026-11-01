// SPDX-License-Identifier: GPL-2.0-or-later

import Testing
@testable import AndariumCore

struct FormatsTests {
    @Test func sizesUseTheUnitThatReadsBest() {
        #expect(Formats.gb(0) == "0 GB")
        #expect(Formats.gb(0.48) == "480 MB")
        #expect(Formats.gb(2) == "2 GB")
        #expect(Formats.gb(2.44) == "2.4 GB")
        #expect(Formats.gb(12.6) == "13 GB")
    }

    @Test func hostSizesAreDecimalAndGuestSizesBinary() {
        #expect(Formats.hostGB(bytes: 1_500_000_000) == "1.5 GB")
        #expect(Formats.guestGB(bytes: Limits.bytesPerGiB * 3) == "3 GB")
    }

    @Test func memoryShowsQuarterStepsWithoutRounding() {
        #expect(Formats.memory(1024) == "1 GB")
        #expect(Formats.memory(1280) == "1.25 GB")
        #expect(Formats.memory(1536) == "1.5 GB")
        #expect(Formats.memory(1792) == "1.75 GB")
        #expect(Formats.memory(3840) == "3.75 GB")
    }

    @Test func elapsedTimePadsSeconds() {
        #expect(Formats.elapsed(0) == "0:00")
        #expect(Formats.elapsed(74) == "1:14")
        #expect(Formats.elapsed(3605) == "60:05")
    }
}

struct MemoryPolicyTests {
    /// The iPad mini 6: iOS lets an application use 2867 MiB.
    let mini = MemoryPolicy(limitMiB: 2867)
    /// A host with more than the runtime accepts to offer.
    let large = MemoryPolicy(limitMiB: 12288)

    @Test func theRecommendedMaximumLeavesTheGraphicsFloor() {
        // 2867 - 82 - 560 - 270 - 384 = 1571, down to the runtime's 64 MiB step.
        #expect(mini.recommendedMaximumMiB == 1536)
        let terms = MemoryTerms.provisional
        let left = mini.limitMiB - terms.appMiB - mini.recommendedMaximumMiB - terms.engineMiB - terms.headroomMiB
        #expect(left >= terms.graphicsFloorMiB)
    }

    @Test func theRulerTopIsOneStepBelowTheRecommendation() {
        #expect(mini.rulerTopMiB == 1280)
        #expect(mini.stops == [1024, 1280])
        #expect(mini.defaultMemoryMiB == 1280)
    }

    @Test func theRuntimeRangeCapsTheRuler() {
        #expect(large.recommendedMaximumMiB == 4096)
        #expect(large.rulerTopMiB == 3840)
        #expect(large.stops.count == 12)
        #expect(large.stops.first == 1024)
        #expect(large.stops.allSatisfy { ($0 - 1024) % Limits.memoryStepMiB == 0 })
        #expect(large.defaultMemoryMiB == 1536)
    }

    @Test func aTinyLimitStillOffersTheFirstPosition() {
        let tiny = MemoryPolicy(limitMiB: 1500)
        #expect(tiny.rulerTopMiB == 1024)
        #expect(tiny.stops == [1024])
        #expect(tiny.defaultMemoryMiB == 1024)
    }

    @Test func aStoredValueSnapsDownOntoTheRuler() {
        #expect(large.rulerValue(1700) == 1536)
        #expect(large.rulerValue(100) == 1024)
        #expect(large.rulerValue(9000) == 3840)
        #expect(mini.rulerValue(2048) == 1280)
    }

    @Test func theRuntimeAcceptsEvery64MiBInItsRange() {
        #expect(MemoryPolicy.runtimeAccepts(1536))
        #expect(MemoryPolicy.runtimeAccepts(512))
        #expect(MemoryPolicy.runtimeAccepts(4096))
        #expect(!MemoryPolicy.runtimeAccepts(1500))
        #expect(!MemoryPolicy.runtimeAccepts(448))
        #expect(!MemoryPolicy.runtimeAccepts(4160))
    }

    @Test func differentTermsMoveTheRuler() {
        let roomier = MemoryTerms(appMiB: 82, engineMiB: 400, headroomMiB: 200, graphicsFloorMiB: 384)
        #expect(MemoryPolicy(limitMiB: 2867, terms: roomier).recommendedMaximumMiB > mini.recommendedMaximumMiB)
    }
}
