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

    /// Info draws the guest's data beside the system and the free space, so
    /// all three are decimal gigabytes: a binary gigabyte is not drawn as one.
    @Test func infoSizesAreDecimalGigabytes() {
        #expect(Formats.decimalGB(bytes: 1_500_000_000) == 1.5)
        #expect(Formats.decimalGB(bytes: Limits.bytesPerGiB) > 1.07)
        #expect(Formats.decimalGB(bytes: Limits.bytesPerGiB) < 1.08)
        let guest = Guest(cores: 2, memoryMiB: 1536, capacityGiB: 8, usedBytes: Limits.bytesPerGiB)
        #expect(guest.effectiveCapacityBytes == 8 * Limits.bytesPerGiB)
        #expect(Formats.hostGB(bytes: guest.effectiveCapacityBytes) == "8.6 GB")
        #expect(Formats.hostGB(bytes: guest.usedBytes) == "1.1 GB")
        var growing = guest
        growing.setCapacity(24)
        #expect(growing.effectiveCapacityBytes == 24 * Limits.bytesPerGiB)
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
    /// A host whose memory limit lets the bound fall as vCPUs are added: 1.5 GB
    /// at four.
    let limited = MemoryPolicy(maximumsMiB: [2048, 1920, 1728, 1536])
    /// A host with no limit to speak of (a Mac).
    let roomy = MemoryPolicy(maximumsMiB: Array(repeating: 4096, count: 8))

    private func host(_ memory: MemoryPolicy, processors: Int, mac: Bool = false) -> HostDevice {
        HostDevice(
            name: mac ? "Mac" : "iPad", processorCount: processors, memory: memory,
            freeStorageBytes: 40_000_000_000, isMac: mac
        )
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
            freeStorageBytes: 40_000_000_000, isMac: true, physicalProcessorCount: 20
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

    @Test func theDefaultIs1536OnADeviceAndTheLargestSizeOnAMac() {
        #expect(roomy.defaultMemoryMiB(vCPUs: 4, onMac: false) == 1536)
        #expect(roomy.defaultMemoryMiB(vCPUs: 4, onMac: true) == 4096)
        // A bound below 1536 wins on a device.
        let tight = MemoryPolicy(maximumsMiB: [1536, 1280])
        #expect(tight.defaultMemoryMiB(vCPUs: 2, onMac: false) == 1280)
        #expect(Guest.fresh(on: host(limited, processors: 4)).memoryMiB == 1536)
        #expect(Guest.fresh(on: host(roomy, processors: 8, mac: true)).memoryMiB == 4096)
        #expect(Guest.fresh(on: host(tight, processors: 2)).memoryMiB == 1280)
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
        #expect(Formats.memoryBound(4096, vCPUs: 10) == "Up to 4 GB")
        #expect(Formats.memoryBound(1536, vCPUs: 4) == "Up to 1.5 GB with 4 vCPUs")
        #expect(Formats.memoryBound(2048, vCPUs: 1) == "Up to 2 GB with 1 vCPU")
    }
}
