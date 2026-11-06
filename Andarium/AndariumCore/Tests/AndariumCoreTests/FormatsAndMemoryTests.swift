// SPDX-License-Identifier: GPL-2.0-or-later

import Testing
@testable import AndariumCore

struct FormatsTests {
    private func bytes(mib: Double) -> UInt64 { UInt64(mib * Double(Limits.bytesPerMiB)) }
    private func bytes(gib: Double) -> UInt64 { UInt64(gib * Double(Limits.bytesPerGiB)) }

    @Test func sizesUseTheUnitThatReadsBest() {
        #expect(Formats.size(bytes: 0) == "0 B")
        #expect(Formats.size(bytes: 512) == "512 B")
        #expect(Formats.size(bytes: 1536) == "1.5 KiB")
        #expect(Formats.size(bytes: bytes(mib: 0.48)) == "492 KiB")
        #expect(Formats.size(bytes: bytes(mib: 480)) == "480 MiB")
        #expect(Formats.size(bytes: bytes(gib: 0.98)) == "1004 MiB")
        #expect(Formats.size(bytes: 2 * Limits.bytesPerGiB) == "2 GiB")
        #expect(Formats.size(bytes: bytes(gib: 1.5)) == "1.5 GiB")
        #expect(Formats.size(bytes: bytes(gib: 12.6)) == "12.6 GiB")
        #expect(Formats.size(bytes: bytes(gib: 118.3)) == "118 GiB")
        #expect(Formats.size(bytes: bytes(gib: 1.82 * 1024)) == "1.82 TiB")
    }

    /// A size keeps three significant digits, so a unit's last values round
    /// up into the next unit and never state "1024 MiB".
    @Test func aSizeRoundingToTheNextUnitMovesUp() {
        #expect(Formats.size(bytes: bytes(mib: 1023.4)) == "1023 MiB")
        #expect(Formats.size(bytes: bytes(mib: 1023.6)) == "1 GiB")
        #expect(Formats.size(bytes: bytes(mib: 9.996)) == "10 MiB")
        #expect(Formats.size(bytes: bytes(mib: 99.96)) == "100 MiB")
        #expect(Formats.size(bytes: bytes(gib: 1023.7)) == "1 TiB")
    }

    @Test func aSizeIsTheSameWhicheverUnitItIsGivenIn() {
        #expect(Formats.size(mib: 8 * 1024) == Formats.size(gib: 8))
        #expect(Formats.size(mib: 1536) == Formats.size(bytes: bytes(gib: 1.5)))
        #expect(Formats.size(gib: 8) == "8 GiB")
        #expect(Formats.size(gib: 64) == "64 GiB")
        #expect(Formats.size(mib: 0) == "0 MiB")
    }

    /// The host's free space, the guest's data and capacity and the system
    /// are all binary: a figure the host reports in bytes is the figure in
    /// gibibytes, not in the decimal gigabytes iOS Settings uses.
    @Test func hostAndGuestSizesShareOneUnit() {
        #expect(Formats.size(bytes: 1_500_000_000) == "1.4 GiB")
        let guest = Guest(cores: 2, memoryMiB: 1536, capacityGiB: 8, usedBytes: Limits.bytesPerGiB)
        #expect(guest.effectiveCapacityBytes == 8 * Limits.bytesPerGiB)
        #expect(Formats.size(bytes: guest.effectiveCapacityBytes) == "8 GiB")
        #expect(Formats.size(bytes: guest.usedBytes) == "1 GiB")
        var growing = guest
        growing.setCapacity(24)
        #expect(growing.effectiveCapacityBytes == 24 * Limits.bytesPerGiB)
        #expect(Formats.size(bytes: growing.effectiveCapacityBytes) == "24 GiB")
    }

    @Test func theGuestsFreeSpaceIsWhatIsLeftOfItsCapacity() {
        let guest = Guest(cores: 2, memoryMiB: 1536, capacityGiB: 8, usedBytes: bytes(gib: 5.4))
        #expect(Formats.size(bytes: guest.freeBytes) == "2.6 GiB")
        var full = guest
        full.usedBytes = 9 * Limits.bytesPerGiB
        #expect(full.freeBytes == 0)
        #expect(Formats.size(bytes: full.freeBytes) == "0 B")
    }

    /// Home states these two figures.
    @Test func theReservesAreStatedInGibibytes() {
        #expect(Formats.size(bytes: Limits.firstStartReserveBytes) == "3 GiB")
        #expect(Formats.size(bytes: Limits.startReserveBytes) == "1.5 GiB")
    }

    @Test func memoryShowsQuarterStepsWithoutRounding() {
        #expect(Formats.size(mib: 512) == "512 MiB")
        #expect(Formats.size(mib: 576) == "576 MiB")
        #expect(Formats.size(mib: 1024) == "1 GiB")
        #expect(Formats.size(mib: 1280) == "1.25 GiB")
        #expect(Formats.size(mib: 1536) == "1.5 GiB")
        #expect(Formats.size(mib: 1792) == "1.75 GiB")
        #expect(Formats.size(mib: 3840) == "3.75 GiB")
        #expect(Formats.size(mib: 4096) == "4 GiB")
    }

    @Test func aRulerLabelIsTheCountOfGibibytes() {
        #expect(Formats.gibibytes(mib: 512) == "0.5")
        #expect(Formats.gibibytes(mib: 1024) == "1")
        #expect(Formats.gibibytes(mib: 1280) == "1.25")
        #expect(Formats.gibibytes(mib: 4096) == "4")
    }

    @Test func elapsedTimePadsSeconds() {
        #expect(Formats.elapsed(0) == "0:00")
        #expect(Formats.elapsed(74) == "1:14")
        #expect(Formats.elapsed(3605) == "60:05")
    }
}

struct MemoryPolicyTests {
    /// A host whose memory limit lets the bound fall as vCPUs are added: 1.5 GiB
    /// at four.
    let limited = MemoryPolicy(maximumsMiB: [2048, 1920, 1728, 1536])
    /// A host with no limit to speak of (a Mac).
    let roomy = MemoryPolicy(maximumsMiB: Array(repeating: 4096, count: 8))

    private func host(_ memory: MemoryPolicy, processors: Int) -> HostDevice {
        HostDevice(name: "iPad", processorCount: processors, memory: memory, freeStorageBytes: 40_000_000_000)
    }

    /// A ten-core device offers the eight vCPUs the memory budget is measured
    /// for: a new guest takes eight, the memory policy has eight rows and the
    /// ruler eight positions, while Info still states the ten cores.
    @Test func aHostWithMoreCoresThanTheLimitOffersTheLimit() {
        var asked: [Int] = []
        let memory = MemoryPolicy(processorCount: 8) { count in
            asked.append(count)
            return 4096
        }
        let device = HostDevice(
            name: "iPad", processorCount: 8, memory: memory, freeStorageBytes: 40_000_000_000,
            physicalProcessorCount: 10
        )
        #expect(asked == Array(1...8))
        #expect(device.processorCount == 8)
        #expect(device.physicalProcessorCount == 10)
        #expect(Guest.fresh(on: device).cores == 8)
        // The overrides the application builds from the same limit reach
        // eight and no further.
        var overrides = LaunchOverrides(processorCount: device.processorCount) {
            $0 == .vCPUCount ? "8" : nil
        }
        #expect(overrides.applied(to: Guest.fresh(on: device), on: device).cores == 8)
        overrides.vCPUCount = 10
        #expect(overrides.applied(to: Guest.fresh(on: device), on: device).cores == 8)
    }

    /// A guest stored with more vCPUs than the host now offers (it was made
    /// when the limit was higher, or on a larger device) comes down to the
    /// limit on load, and its memory to the bound of that count.
    @Test func aStoredCountAboveTheLimitComesDownOnLoad() {
        let device = HostDevice(
            name: "Mac", processorCount: 8,
            memory: MemoryPolicy(maximumsMiB: [4096, 4096, 4096, 4096, 4096, 4096, 3840, 3584]),
            freeStorageBytes: 40_000_000_000, physicalProcessorCount: 20
        )
        let stored = Guest(cores: 10, memoryMiB: 4096, capacityGiB: 8)
        let loaded = LaunchOverrides().applied(to: stored, on: device)
        #expect(loaded.cores == 8 && loaded.memoryMiB == 3584)
        // A count within the limit is kept, and so is a stored zero's floor of one.
        #expect(LaunchOverrides().applied(to: Guest(cores: 4, memoryMiB: 2048, capacityGiB: 8), on: device).cores == 4)
        #expect(LaunchOverrides().applied(to: Guest(cores: 0, memoryMiB: 2048, capacityGiB: 8), on: device).cores == 1)
    }

    @Test func aHostsOwnProcessorsDefaultToTheLimitAndNeverFallBelowIt() {
        let six = host(roomy, processors: 6)
        #expect(six.processorCount == 6 && six.physicalProcessorCount == 6)
        let odd = HostDevice(
            name: "iPad", processorCount: 6, memory: roomy, freeStorageBytes: 0, physicalProcessorCount: 2)
        #expect(odd.physicalProcessorCount == 6)
        #expect(Guest.fresh(on: six).cores == 6)
    }

    @Test func thePickerRunsFrom512ToTheBoundOfTheVCPUCountInSteps64() {
        let four = limited.stops(vCPUs: 4)
        #expect(four.first == 512 && four.last == 1536)
        #expect(four.count == (1536 - 512) / 64 + 1)
        #expect(four.allSatisfy(MemoryPolicy.runtimeAccepts))
        #expect(limited.stops(vCPUs: 1).last == 2048)
        #expect(roomy.stops(vCPUs: 8).count == 57)
        // Added vCPUs never raise the bound.
        let bounds = (1...4).map { limited.maximumMiB(vCPUs: $0) }
        #expect(bounds == bounds.sorted(by: >))
    }

    @Test func theBoundsComeOntoTheRuntimesStepsAndRange() {
        let policy = MemoryPolicy(maximumsMiB: [100, 5000, 1700])
        #expect(policy.maximumMiB(vCPUs: 1) == 512)
        #expect(policy.maximumMiB(vCPUs: 2) == 4096)
        #expect(policy.maximumMiB(vCPUs: 3) == 1664)
        // A count above the host's takes the last bound; zero counts as one.
        #expect(policy.maximumMiB(vCPUs: 9) == 1664)
        #expect(policy.maximumMiB(vCPUs: 0) == 512)
        #expect(MemoryPolicy(maximumsMiB: []).maximumMiB(vCPUs: 1) == 4096)
    }

    @Test func theLoaderBoundIsReadOncePerVCPUCount() {
        var asked: [Int] = []
        let policy = MemoryPolicy(processorCount: 3) { count in
            asked.append(count)
            return 4096 - count * 128
        }
        #expect(asked == [1, 2, 3])
        #expect(policy.maximumMiB(vCPUs: 2) == 3840)
    }

    /// The bounds the shared budget formula (`runtime/guest_memory_budget.hpp`,
    /// `maximum_guest_memory_mib`) gives at 1 through 8 vCPUs for a process
    /// memory limit, and the default a new guest then takes at 4, 6 and 8
    /// vCPUs: the bound rounded down to 0.5 GiB, held within 1.5 to 4 GiB,
    /// and never above the bound. `AndariumTests` asks the loader for the same
    /// bounds, so these rows stay the formula's.
    static let defaultFixtures: [(limitMiB: Int, bounds: [Int], defaults: [Int: Int])] = [
        // Below 1.5 GiB the bound wins: the safe limit is never exceeded.
        (1536, [960, 960, 896, 896, 896, 896, 896, 896], [4: 896, 6: 896, 8: 896]),
        (2048, [1472, 1472, 1408, 1408, 1344, 1344, 1344, 1344], [4: 1408, 6: 1344, 8: 1344]),
        // The lower clamp: 1664 and 1600 round down to 1536.
        (2304, [1728, 1728, 1664, 1664, 1600, 1600, 1600, 1600], [4: 1536, 6: 1536, 8: 1536]),
        // An iPad mini 6's 2867 MiB.
        (2867, [2304, 2304, 2240, 2240, 2176, 2176, 2176, 2176], [4: 2048, 6: 2048, 8: 2048]),
        (3584, [3008, 3008, 2944, 2944, 2880, 2880, 2880, 2880], [4: 2560, 6: 2560, 8: 2560]),
        (4096, [3520, 3520, 3456, 3456, 3392, 3392, 3392, 3392], [4: 3072, 6: 3072, 8: 3072]),
        // The upper clamp: a limit that leaves the whole range, and a Mac with none.
        (5120, Array(repeating: 4096, count: 8), [4: 4096, 6: 4096, 8: 4096]),
        (0, Array(repeating: 4096, count: 8), [4: 4096, 6: 4096, 8: 4096]),
    ]

    @Test(arguments: [4, 6, 8])
    func aNewGuestTakesEveryCoreAndTheSafeLimitInHalfGiBSteps(cores: Int) {
        for fixture in Self.defaultFixtures {
            let policy = MemoryPolicy(maximumsMiB: fixture.bounds)
            let device = host(policy, processors: cores)
            let guest = Guest.fresh(on: device)
            #expect(guest.cores == cores)
            #expect(guest.memoryMiB == fixture.defaults[cores], "limit \(fixture.limitMiB) MiB")
            #expect(guest.memoryMiB == policy.defaultMemoryMiB(vCPUs: cores))
            // Never above the safe limit, on the runtime's steps, and
            // whole half gibibytes wherever the clamp did not take the bound.
            #expect(guest.memoryMiB <= policy.maximumMiB(vCPUs: cores))
            #expect(MemoryPolicy.runtimeAccepts(guest.memoryMiB))
            if guest.memoryMiB < policy.maximumMiB(vCPUs: cores) {
                #expect(guest.memoryMiB % Limits.defaultMemoryStepMiB == 0)
            }
            if policy.maximumMiB(vCPUs: cores) >= Limits.defaultMemoryRangeMiB.lowerBound {
                #expect(Limits.defaultMemoryRangeMiB.contains(guest.memoryMiB))
            }
        }
    }

    @Test func theDefaultFollowsTheVCPUCountWhenTheBoundFalls() {
        // 4096 MiB limit: the bound falls from 3520 at one vCPU to 3392 at six.
        let policy = MemoryPolicy(maximumsMiB: [3520, 3520, 3456, 3456, 3392, 3392, 3392, 3392])
        #expect(policy.defaultMemoryMiB(vCPUs: 1) == 3072)
        #expect(policy.defaultMemoryMiB(vCPUs: 8) == 3072)
        // A bound just under a half-gibibyte step steps down to the one below.
        #expect(MemoryPolicy(maximumsMiB: [3520, 3071]).defaultMemoryMiB(vCPUs: 2) == 2560)
        #expect(MemoryPolicy(maximumsMiB: [3520, 3072]).defaultMemoryMiB(vCPUs: 2) == 3072)
    }

    @Test func aStoredSizeIsTakenWithinTheBoundOnLoad() {
        let device = host(limited, processors: 4)
        // Above the bound of its vCPU count: down to the bound.
        #expect(Guest(cores: 4, memoryMiB: 3072, capacityGiB: 8).memoryWithinBound(on: device).memoryMiB == 1536)
        // The same size is allowed with fewer vCPUs.
        #expect(Guest(cores: 1, memoryMiB: 2048, capacityGiB: 8).memoryWithinBound(on: device).memoryMiB == 2048)
        // Off the steps: the nearest step, then the bound.
        #expect(Guest(cores: 2, memoryMiB: 1700, capacityGiB: 8).memoryWithinBound(on: device).memoryMiB == 1728)
        #expect(Guest(cores: 2, memoryMiB: 100, capacityGiB: 8).memoryWithinBound(on: device).memoryMiB == 512)
        #expect(Guest(cores: 2, memoryMiB: 1024, capacityGiB: 8).memoryWithinBound(on: device).memoryMiB == 1024)
    }

    @Test func raisingVCPUsBringsMemoryDownInStepsAndLoweringLeavesIt() {
        let device = host(limited, processors: 4)
        var guest = Guest(cores: 1, memoryMiB: 2048, capacityGiB: 8)
        guest.setCores(2, on: device)
        #expect(guest.cores == 2 && guest.memoryMiB == 1920)
        guest.setCores(3, on: device)
        #expect(guest.memoryMiB == 1728)
        guest.setCores(4, on: device)
        #expect(guest.memoryMiB == 1536)
        #expect(guest.memoryMiB % Limits.memoryStepMiB == 0)
        // Lowering does not give memory back.
        guest.setCores(1, on: device)
        #expect(guest.cores == 1 && guest.memoryMiB == 1536)
        // Raising with memory already below the new bound leaves it too.
        var small = Guest(cores: 1, memoryMiB: 1024, capacityGiB: 8)
        small.setCores(4, on: device)
        #expect(small.memoryMiB == 1024)
    }

    @Test func anAutomationLaunchStatesItsSizeUnclamped() {
        let device = host(limited, processors: 4)
        var overrides = LaunchOverrides()
        overrides.memoryMiB = 4096
        overrides.vCPUCount = 4
        let stated = overrides.applied(to: Guest.fresh(on: device), on: device)
        #expect(stated.cores == 4 && stated.memoryMiB == 4096)
        // A launch that states only vCPUs takes the stored size within the
        // bound of the count it states.
        var onlyCPUs = LaunchOverrides()
        onlyCPUs.vCPUCount = 4
        let stored = Guest(cores: 1, memoryMiB: 2048, capacityGiB: 8)
        #expect(onlyCPUs.applied(to: stored, on: device).memoryMiB == 1536)
        // And a launch that states nothing clamps what is stored.
        let big = Guest(cores: 4, memoryMiB: 3072, capacityGiB: 8)
        #expect(LaunchOverrides().applied(to: big, on: device).memoryMiB == 1536)
    }

    @Test func theRuntimeAcceptsEvery64MiBInItsRange() {
        #expect(MemoryPolicy.runtimeAccepts(1536))
        #expect(MemoryPolicy.runtimeAccepts(512))
        #expect(MemoryPolicy.runtimeAccepts(4096))
        #expect(!MemoryPolicy.runtimeAccepts(1500))
        #expect(!MemoryPolicy.runtimeAccepts(448))
        #expect(!MemoryPolicy.runtimeAccepts(4160))
    }

    @Test func theBoundIsWordedWithTheCountWhereTheLimitMattersOnly() {
        #expect(Formats.memoryBound(4096, vCPUs: 10) == "Up to 4 GiB")
        #expect(Formats.memoryBound(1536, vCPUs: 4) == "Up to 1.5 GiB with 4 vCPUs")
        #expect(Formats.memoryBound(2048, vCPUs: 1) == "Up to 2 GiB with 1 vCPU")
    }
}
