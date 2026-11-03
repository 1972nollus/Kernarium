// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// The pieces of the device application that touch the host: the release it
/// carries, the console it reads and the options it keeps.
struct SystemFolderTests {
    private static let manifest = #"{"release_id": "20261031-los24-r4", "android": {"fingerprint": "Android/x:17/a/b:userdebug/release-keys", "build_number": 1793463711, "security_patch": "2026-10-01"}}"#

    private func layout(size: Int) -> String {
        #"{"system_disk": {"size_bytes": \#(size), "sha256": "\#(String(repeating: "a", count: 64))"}, "data_disk": {"layout_version": 1, "capacity_gib": {"minimum": 4, "default": 8, "maximum": 64}}}"#
    }

    /// A release directory with a system disk of `disk` bytes.
    private func folder(disk: Int = 4096, declared: Int = 4096, omit: String? = nil) throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("andarium-system-\(UUID().uuidString)")
        let files: [String: Data] = [
            SystemFolder.manifest: Data(Self.manifest.utf8),
            SystemFolder.commandLine: Data("init=/init\n".utf8),
            SystemFolder.layout: Data(layout(size: declared).utf8),
            SystemFolder.systemDisk: Data(count: disk),
        ]
        for (name, data) in files where name != omit {
            let file = url.appendingPathComponent(name)
            try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            try data.write(to: file)
        }
        return url
    }

    @Test func theReleaseIsTheDirectoryWithItsSystemDisk() throws {
        let url = try folder()
        defer { try? FileManager.default.removeItem(at: url) }
        let release = try SystemFolder.release(in: url)
        #expect(release.identity.releaseID == "20261031-los24-r4")
        #expect(release.identity.buildNumber == 1_793_463_711)
        #expect(release.systemDiskBytes == 4096)
    }

    @Test func aFileThatIsMissingIsNamed() throws {
        for name in [SystemFolder.manifest, SystemFolder.commandLine, SystemFolder.layout, SystemFolder.systemDisk] {
            let url = try folder(omit: name)
            defer { try? FileManager.default.removeItem(at: url) }
            #expect(throws: SystemFolder.Problem.missing(name)) { try SystemFolder.release(in: url) }
        }
    }

    @Test func aSystemDiskOfAnotherSizeIsRefused() throws {
        let url = try folder(disk: 4096, declared: 8192)
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(throws: SystemFolder.Problem.sizeDiffers(declared: 8192, actual: 4096)) { try SystemFolder.release(in: url) }
    }
}

struct ConsoleSinkTests {
    private nonisolated final class Collector: @unchecked Sendable {
        private let lock = NSLock()
        private var found: [BootMilestone] = []
        private var completions = 0
        var milestones: [BootMilestone] { lock.withLock { found } }
        var bootCompletions: Int { lock.withLock { completions } }
        func add(_ more: [BootMilestone], _ completed: Bool) {
            lock.withLock {
                found += more
                if completed { completions += 1 }
            }
        }
    }

    @Test func milestonesArriveFromAnyThreadInOrder() async {
        let collector = Collector()
        let sink = ConsoleSink { collector.add($0, $1) }
        sink.feed(Data("Linux ver".utf8))
        await Task.detached { sink.feed(Data("sion 6.18\ninit: init first stage started!\n".utf8)) }.value
        #expect(collector.milestones == [.kernelRunning, .androidStarting])
        #expect(collector.bootCompletions == 0)
    }

    @Test func onlyAndroidBootingIsABootCompletion() {
        let collector = Collector()
        let sink = ConsoleSink { collector.add($0, $1) }
        sink.feed(Data("init: starting service 'recovery'...\n".utf8))
        #expect(collector.milestones == [.ready] && collector.bootCompletions == 0)
        sink.feed(Data("init: processing action (sys.boot_completed=1) from (/x:1)\n".utf8))
        #expect(collector.bootCompletions == 1)
    }
}

struct DisplayOptionsStoreTests {
    @Test func optionsSurviveTheStore() {
        let saved = DisplayOptionsStore.load()
        defer { DisplayOptionsStore.save(saved) }
        DisplayOptionsStore.save(DisplayOptions(retina: true, followWindow: false))
        #expect(DisplayOptionsStore.load() == DisplayOptions(retina: true, followWindow: false))
    }
}

struct HostDeviceTests {
    @Test func theHostReadsThisDevice() {
        let host = AppEntry.hostDevice()
        #expect(host.processorCount >= 1)
        // The bound comes from the runtime's budget formula through the loader:
        // inside the runtime's range, never rising with the vCPU count, and the
        // largest size where the system sets no limit (a Mac).
        let bounds = (1...host.processorCount).map { host.memory.maximumMiB(vCPUs: $0) }
        #expect(bounds.allSatisfy { Limits.runtimeMemoryRangeMiB.contains($0) })
        #expect(bounds == bounds.sorted(by: >))
        #expect(host.isMac == ProcessInfo.processInfo.isiOSAppOnMac)
        if host.isMac { #expect(bounds.allSatisfy { $0 == Limits.runtimeMemoryRangeMiB.upperBound }) }
        #expect(AppEntry.freeStorageBytes() > 0)
    }
}

/// The system Andarium carries, written the way Create writes it: decoded
/// sparse from the bundled archive and grown to the guest's size. It is the
/// one test that reads the real archive, so it takes tens of seconds.
/// A harness launch on a Mac starts Andarium through `xcodebuild test`, which
/// ends the application with the test run. An automation launch that sets
/// `ANDARIUM_HOLD_SECONDS` keeps the host up that long, so its guest can boot
/// and run; without it the test returns at once.
struct AutomationHoldTests {
    @Test func anAutomationLaunchHoldsTheHostForTheRequestedTime() async throws {
        guard let text = ProcessInfo.processInfo.environment["ANDARIUM_HOLD_SECONDS"], let seconds = Double(text), seconds > 0 else { return }
        try await Task.sleep(for: .seconds(seconds))
    }
}
