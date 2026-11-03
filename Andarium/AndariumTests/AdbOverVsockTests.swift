// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
@preconcurrency import Security
import Testing
@testable import Andarium

/// The application's own adb client against a real Android guest, with
/// authorization on. It runs only when the launch asks, as the hold test does:
///
///     xcodebuild test -scheme Andarium -destination 'platform=macOS,variant=Designed for iPad' \
///         -only-testing:AndariumTests/AdbOverVsockTests \
///         TEST_RUNNER_ANDARIUM_ADB_TESTS=1 TEST_RUNNER_ANDARIUM_START=normal
///
/// The application boots the guest as it launches; the tests wait for Android
/// and use the key the application made for that boot.
@Suite(.serialized)
struct AdbOverVsockTests {
    private static var enabled: Bool { ProcessInfo.processInfo.environment["ANDARIUM_ADB_TESTS"] == "1" }

    /// The same code the screens run, for the key of this boot.
    private func operations() throws -> AdbGuestOperations {
        let identity = try AdbKeyStore().loadOrCreate()
        return AdbGuestOperations(identity: identity, clock: GuestRunClock(), device: "device")
    }

    /// Waits, up to 15 minutes, until Android has booted and adbd answers.
    private func awaitAndroid(_ operations: AdbGuestOperations) async throws {
        let deadline = ContinuousClock.now + .seconds(15 * 60)
        while ContinuousClock.now < deadline {
            if let booted = try? await operations.shell("getprop sys.boot_completed"), booted.text.hasPrefix("1") {
                return
            }
            try await Task.sleep(for: .seconds(5))
        }
        Issue.record("Android did not finish booting in 15 minutes")
        throw CancellationError()
    }

    @Test(.timeLimit(.minutes(20))) func theClientReachesAdbdAndRunsACommand() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        try await awaitAndroid(operations)
        let result = try await operations.shell("getprop ro.build.version.sdk; echo done")
        #expect(result.exitCode == 0)
        #expect(result.text.hasSuffix("done\n"))
        // The features the client asks for are the ones adbd has.
        let session = AdbSession(
            connector: VsockConnector(), signer: operations.identity?.signer, clock: operations.clock)
        let banner = try session.run { $0.banner }
        #expect(banner.features.isSuperset(of: ["shell_v2", "cmd", "stat_v2", "abb_exec"]))
    }

    @Test(.timeLimit(.minutes(30))) func aBugReportIsAZipOfMoreThanAMegabyte() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        try await awaitAndroid(operations)
        let started = ContinuousClock.now
        let progress = Progress()
        let file = try await operations.createBugReport { progress.add($0) }
        let seconds = (ContinuousClock.now - started).components.seconds
        print("ADBVSOCK: bug report made in \(seconds) s")
        defer { try? FileManager.default.removeItem(at: file) }
        let size = try #require(try file.resourceValues(forKeys: [.fileSizeKey]).fileSize)
        #expect(size > 1_000_000)
        #expect(try FileHandle(forReadingFrom: file).read(upToCount: 4) == Data([0x50, 0x4B, 0x03, 0x04]))
        #expect(file.deletingLastPathComponent() == BugReportStaging.directory)
        #expect(progress.values == progress.values.sorted())
        #expect((progress.values.last ?? 0) > 0.9)
    }

    @Test(.timeLimit(.minutes(20))) func cancellingABugReportStopsItAndLeavesNothing() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        try await awaitAndroid(operations)
        let report = Task { try await operations.createBugReport { _ in } }
        try await Task.sleep(for: .seconds(15))
        report.cancel()
        let started = ContinuousClock.now
        do {
            _ = try await report.value
            Issue.record("the report finished before the cancel")
        } catch is CancellationError {
        }
        print("ADBVSOCK: bug report cancelled in \((ContinuousClock.now - started).components.seconds) s")
        // dumpstatez is stopped, and no staged file stays.
        var state = ""
        for _ in 0..<30 {
            state = try await operations.shell("getprop init.svc.dumpstatez").text.trimmingCharacters(in: .whitespacesAndNewlines)
            if state != "running" { break }
            try await Task.sleep(for: .seconds(1))
        }
        #expect(state != "running")
        let staged = (try? FileManager.default.contentsOfDirectory(atPath: BugReportStaging.directory.path)) ?? []
        #expect(staged.isEmpty)
    }

    /// The key the application made for this boot changes when the guest is
    /// erased. A first launch records its line in the file the run names; a
    /// launch that erased the guest first (ANDARIUM_ERASE=1) must differ.
    @Test func eraseGuestMakesANewKey() throws {
        guard Self.enabled, let path = ProcessInfo.processInfo.environment["ANDARIUM_ADB_KEY_FILE"] else { return }
        let line = try AdbKeyStore().loadOrCreate().publicKeyLine
        let file = URL(fileURLWithPath: path)
        if let previous = try? String(contentsOf: file, encoding: .utf8) {
            #expect(previous != line)
        }
        try line.write(to: file, atomically: true, encoding: .utf8)
    }

    @Test(.timeLimit(.minutes(20))) func aSystemApkPulledFromTheImageInstallsAndTheUninstallRestoresIt() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        try await awaitAndroid(operations)
        // The smallest base APKs of system apps that are not privileged.
        let listing = try await operations.shell("pm list packages -s -f").text
        let apps = listing.split(separator: "\n").compactMap { line -> (path: String, package: String)? in
            guard line.hasPrefix("package:"), let equals = line.lastIndex(of: "=") else { return nil }
            let path = String(line[line.index(line.startIndex, offsetBy: 8)..<equals])
            guard path.hasSuffix(".apk"), path.contains("/app/"), !path.contains("/priv-app/") else { return nil }
            return (path, String(line[line.index(after: equals)...]))
        }
        #expect(!apps.isEmpty)
        let quoted = apps.map { "'\($0.path)'" }.joined(separator: " ")
        let sizes = try await operations.shell("stat -c '%s %n' \(quoted)").text
        let ordered = sizes.split(separator: "\n").compactMap { line -> (size: Int, path: String)? in
            let parts = line.split(separator: " ", maxSplits: 1)
            guard parts.count == 2, let size = Int(parts[0]), size > 50_000 else { return nil }
            return (size, String(parts[1]))
        }.sorted { $0.size < $1.size }
        var outcomes: [String] = []
        for candidate in ordered.prefix(5) {
            let package = try #require(apps.first { $0.path == candidate.path }).package
            let local = FileManager.default.temporaryDirectory.appendingPathComponent("pulled-\(UUID().uuidString).apk")
            defer { try? FileManager.default.removeItem(at: local) }
            try await pull(candidate.path, to: local, operations: operations)
            do {
                try await operations.installApk(at: local, sizeBytes: candidate.size) { _ in }
            } catch {
                outcomes.append("\(package): \(error.localizedDescription)")
                continue
            }
            // The update is in /data/app; removing it restores the system's copy.
            let path = try await operations.shell("pm path \(package)").text
            #expect(path.contains("/data/app/"), "\(path)")
            let removal = try await operations.shell("pm uninstall \(package)")
            #expect(removal.text.contains("Success"), "\(removal.text)")
            let restored = try await operations.shell("pm path \(package)").text
            #expect(restored.contains(candidate.path), "\(restored)")
            return
        }
        Issue.record("no system APK took an update: \(outcomes)")
    }

    private func pull(_ remote: String, to local: URL, operations: AdbGuestOperations) async throws {
        FileManager.default.createFile(atPath: local.path, contents: nil)
        let handle = try FileHandle(forWritingTo: local)
        defer { try? handle.close() }
        let sink = HandleSink(handle)
        let identity = try #require(operations.identity)
        try await AdbOperationRunner.run(identity: identity, clock: operations.clock) { session in
            try session.run { connection in
                session.watchdog.arm(idle: .seconds(60), overall: .seconds(600))
                let sync = try AdbSync(connection: connection)
                defer { sync.quit() }
                try sync.receive(remote) { try sink.write($0) }
            }
        }
    }
}

/// Collects a bug report's progress from the operation's thread.
private nonisolated final class Progress: @unchecked Sendable {
    private let lock = NSLock()
    private var all: [Double] = []

    var values: [Double] { lock.withLock { all } }

    func add(_ update: BusyUpdate) {
        guard let fraction = update.progress else { return }
        lock.withLock { all.append(fraction) }
    }
}

private nonisolated final class HandleSink: @unchecked Sendable {
    private let handle: FileHandle

    init(_ handle: FileHandle) {
        self.handle = handle
    }

    func write(_ data: Data) throws {
        try handle.write(contentsOf: data)
    }
}
