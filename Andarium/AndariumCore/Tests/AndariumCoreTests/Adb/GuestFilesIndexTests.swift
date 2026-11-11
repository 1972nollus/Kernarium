// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

private let root = GuestFilePath.root

/// The media index's side of an import: the scan that follows the move, and
/// the recovery of rows that earlier builds left incomplete.
struct GuestFilesIndexTests {
    @Test func theScanNamesTheFileAsAnEncodedUri() {
        #expect(GuestFiles.scanCommand(for: root + "/Download/Clip.mov")
            == "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d 'file://\(root)/Download/Clip.mov'")
        // Bytes a URI reads for something else, and the quote the shell does.
        #expect(GuestFiles.uriPath("/a b#c%d?e'f(g)&h+i;j é/日本.cia")
            == "/a%20b%23c%25d%3Fe%27f%28g%29%26h%2Bi%3Bj%20%C3%A9/%E6%97%A5%E6%9C%AC.cia")
        #expect(GuestFiles.scanCommand(for: root + "/Download/Captain Toad-Treasure Tracker(USA).cia")
            .hasSuffix("-d 'file://\(root)/Download/Captain%20Toad-Treasure%20Tracker%28USA%29.cia'"))
    }

    @Test func theFinishCommandMovesThenScansOnlyAfterTheMove() {
        let command = GuestFiles.finishCommand(
            temporary: root + "/D/.andarium-1.part", path: root + "/D/a b.cia", replacing: false)
        let move = "mv -- '\(root)/D/.andarium-1.part' '\(root)/D/a b.cia'"
        let scan = "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d 'file://\(root)/D/a%20b.cia'"
        #expect(command.hasSuffix("\(move) && { \(scan) >/dev/null 2>&1; true; }"))
        // Nothing before the move can scan, and the scan is the last thing.
        let before = command.components(separatedBy: move)[0]
        #expect(!before.contains("am broadcast"))
        #expect(command.components(separatedBy: "am broadcast").count == 2)
    }

    // MARK: The command as a shell runs it

    /// What `finishCommand` does in a real shell, with `mv` as it is and `am`
    /// a script that notes its arguments, in a folder of its own.
    private struct Harness {
        let directory: URL
        var bin: URL { directory.appendingPathComponent("bin") }
        var calls: URL { directory.appendingPathComponent("calls.txt") }

        init() throws {
            directory = FileManager.default.temporaryDirectory
                .appendingPathComponent("FinishCommand-\(UUID().uuidString)", isDirectory: true)
            try FileManager.default.createDirectory(at: bin, withIntermediateDirectories: true)
            let script = bin.appendingPathComponent("am")
            try """
                #!/bin/sh
                for argument in "$@"; do printf '%s\\n' "$argument" >> "\(calls.path)"; done
                printf -- '---\\n' >> "\(calls.path)"
                exit ${AM_EXIT:-0}
                """.write(to: script, atomically: true, encoding: .utf8)
            try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: script.path)
        }

        func remove() { try? FileManager.default.removeItem(at: directory) }

        func path(_ name: String) -> String { directory.appendingPathComponent(name).path }

        func write(_ name: String, _ text: String) throws {
            try text.write(toFile: path(name), atomically: true, encoding: .utf8)
        }

        func read(_ name: String) -> String? { try? String(contentsOfFile: path(name), encoding: .utf8) }

        func exists(_ name: String) -> Bool { FileManager.default.fileExists(atPath: path(name)) }

        /// The arguments of each `am` that ran.
        var scans: [[String]] {
            guard let text = try? String(contentsOf: calls, encoding: .utf8) else { return [] }
            return text.components(separatedBy: "---\n").dropLast().map {
                $0.split(separator: "\n", omittingEmptySubsequences: false).dropLast().map(String.init)
            }
        }

        /// Runs `command` in a shell and returns its exit status.
        func run(_ command: String, amExit: Int = 0) throws -> Int32 {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/bin/sh")
            process.arguments = ["-c", command]
            process.environment = ["PATH": "\(bin.path):/usr/bin:/bin", "AM_EXIT": String(amExit)]
            process.standardOutput = FileHandle.nullDevice
            process.standardError = FileHandle.nullDevice
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus
        }

        func finish(replacing: Bool, to name: String = "final.cia", amExit: Int = 0) throws -> Int32 {
            try run(
                GuestFiles.finishCommand(temporary: path(".andarium-1.part"), path: path(name), replacing: replacing),
                amExit: amExit)
        }

        /// What one scan of the file called `name` passes to `am`.
        func scanned(_ name: String) -> [[String]] {
            [[
                "broadcast", "-a", "android.intent.action.MEDIA_SCANNER_SCAN_FILE", "-d",
                "file://" + GuestFiles.uriPath(path(name)),
            ]]
        }
    }

    @Test func aMoveThatWorkedIsFollowedByTheScanOfThatFile() throws {
        let harness = try Harness()
        defer { harness.remove() }
        try harness.write(".andarium-1.part", "body")
        #expect(try harness.finish(replacing: false) == 0)
        #expect(harness.read("final.cia") == "body" && !harness.exists(".andarium-1.part"))
        #expect(harness.scans == harness.scanned("final.cia"))
    }

    @Test func aNameWithSpacesAndQuotesIsMovedAndScannedWhole() throws {
        let harness = try Harness()
        defer { harness.remove() }
        let name = "it's a #1 (copy) 100%.cia"
        try harness.write(".andarium-1.part", "body")
        #expect(try harness.finish(replacing: false, to: name) == 0)
        #expect(harness.read(name) == "body")
        #expect(harness.scans == harness.scanned(name))
        #expect(harness.scans.first?.last?.contains("%20") == true)
    }

    @Test func aNameThatIsTakenIsRefusedAndNothingIsScanned() throws {
        let harness = try Harness()
        defer { harness.remove() }
        try harness.write(".andarium-1.part", "new")
        try harness.write("final.cia", "old")
        #expect(try harness.finish(replacing: false) == Int32(GuestFiles.exitExists))
        #expect(harness.read("final.cia") == "old" && harness.read(".andarium-1.part") == "new")
        #expect(harness.scans.isEmpty)
    }

    @Test func aFolderInTheWayOfAReplaceIsRefusedAndNothingIsScanned() throws {
        let harness = try Harness()
        defer { harness.remove() }
        try harness.write(".andarium-1.part", "new")
        try FileManager.default.createDirectory(atPath: harness.path("final.cia"), withIntermediateDirectories: false)
        #expect(try harness.finish(replacing: true) == Int32(GuestFiles.exitFolder))
        #expect(harness.exists(".andarium-1.part") && harness.scans.isEmpty)
    }

    @Test func aReplaceOfAnIndexedFileScansTheNewOne() throws {
        let harness = try Harness()
        defer { harness.remove() }
        try harness.write(".andarium-1.part", "new")
        try harness.write("final.cia", "old")
        #expect(try harness.finish(replacing: true) == 0)
        #expect(harness.read("final.cia") == "new")
        #expect(harness.scans == harness.scanned("final.cia"))
    }

    @Test func aMoveThatFailsKeepsItsStatusAndNeverScans() throws {
        let harness = try Harness()
        defer { harness.remove() }
        // No partial file: `mv` fails, and its status is the command's.
        #expect(try harness.finish(replacing: false) != 0)
        #expect(!harness.exists("final.cia") && harness.scans.isEmpty)
    }

    @Test func aScanThatFailsDoesNotFailTheImport() throws {
        let harness = try Harness()
        defer { harness.remove() }
        try harness.write(".andarium-1.part", "body")
        #expect(try harness.finish(replacing: false, amExit: 1) == 0)
        #expect(harness.read("final.cia") == "body")
        #expect(harness.scans.count == 1)
    }

    // MARK: Recovery

    @Test func theQueryAsksForTheShellsPendingRowsUnderInternalStorage() {
        let query = GuestFiles.unfinishedIndexQuery
        #expect(query.hasPrefix("content query --uri content://media/external/file --projection _data --where '"))
        #expect(query.contains("is_pending=1 AND owner_package_name="))
        #expect(query.contains("com.android.shell"))
        #expect(query.contains("_data LIKE "))
        #expect(query.contains("\(root)/%"))
        #expect(query.hasSuffix(" | head -n 2000"))
    }

    @Test func theFilesToCompleteComeOutOfTheQueryOutput() {
        let output = """
            Row: 0 _data=\(root)/Download/Captain Toad-Treasure Tracker(USA).cia
            Row: 1 _data=\(root)/Download/Winlator_11.2.apk
            Row: 2 _data=\(root)/Download/.andarium-c88d7783.part
            Row: 3 _data=/data/media/0/elsewhere.bin
            Row: 4 _data=\(root)/Download/a, b=c.zip
            Row: 5 _data=\(root)/../escape.bin
            a continuation line
            Row: 6 _data=\(root)/Music/song.mp3

            """
        #expect(GuestFiles.filesWithUnfinishedIndex(in: output) == [
            root + "/Download/Captain Toad-Treasure Tracker(USA).cia",
            root + "/Download/Winlator_11.2.apk",
            root + "/Download/a, b=c.zip",
            root + "/Music/song.mp3",
        ])
        #expect(GuestFiles.filesWithUnfinishedIndex(in: "No result found.\n").isEmpty)
        #expect(GuestFiles.filesWithUnfinishedIndex(in: "").isEmpty)
    }

    @Test func aRecoveryCompletesOnlyAsManyFilesAsItsLimit() {
        let output = (0..<(GuestFiles.unfinishedIndexLimit + 50))
            .map { "Row: \($0) _data=\(root)/Download/f\($0).bin" }.joined(separator: "\n")
        let files = GuestFiles.filesWithUnfinishedIndex(in: output)
        #expect(files.count == GuestFiles.unfinishedIndexLimit)
        #expect(files.first == root + "/Download/f0.bin")
    }

    @Test func theScansGoInBatchesOfFiles() {
        let paths = (0..<60).map { "\(root)/Download/f\($0).bin" }
        let batches = GuestFiles.scanBatches(for: paths)
        #expect(batches.count == 3)
        #expect(batches.map { $0.components(separatedBy: "; ").count } == [25, 25, 10])
        #expect(batches[0].hasPrefix(GuestFiles.scanCommand(for: paths[0]) + "; "))
        #expect(GuestFiles.scanBatches(for: []).isEmpty)
        #expect(GuestFiles.scanBatches(for: [paths[0]]) == [GuestFiles.scanCommand(for: paths[0])])
    }
}
