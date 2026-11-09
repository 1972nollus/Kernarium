// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

struct InternalStorageTests {
    @Test func theCommandAnswersReadyOrStarting() {
        #expect(InternalStorage.parse("ready\n") == true)
        #expect(InternalStorage.parse("starting\n") == false)
        #expect(InternalStorage.parse("") == nil)
        #expect(InternalStorage.parse("/system/bin/sh: grep: not found") == nil)
    }

    @Test func aFailedCommandSaysNothing() {
        #expect(InternalStorage.parse(ShellResult(stdout: Data("ready\n".utf8), exitCode: 0)) == true)
        #expect(InternalStorage.parse(ShellResult(stdout: Data("starting\n".utf8), exitCode: 0)) == false)
        #expect(InternalStorage.parse(ShellResult(stdout: Data("ready\n".utf8), exitCode: 1)) == nil)
        #expect(InternalStorage.parse(ShellResult(stdout: Data(), exitCode: nil)) == nil)
    }

    @Test func theCommandIsOneLineThatNamesTheMountAndTheFolders() {
        #expect(!InternalStorage.command.contains("\n"))
        for part in ["/proc/mounts", "/storage/emulated fuse", "-w /storage/emulated/0", "Download", "DCIM", "Pictures"] {
            #expect(InternalStorage.command.contains(part), "\(part)")
        }
    }
}
