// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The runtime's record of how the previous session ended, read as the
/// application reads it at launch.
struct SessionRecordTests {
    @Test func readsAPanicWithItsKernelOutput() throws {
        let text = """
            uml-session 1
            state ended
            end guest-panic
            status 6
            pid 46851
            started 1794114290
            ended 1794114295
            message sysrq triggered crash
            kernel-output
            [    0.566837] Kernel panic - not syncing: sysrq triggered crash
            Darwin UML panic: sysrq triggered crash

            """
        let record = try #require(SessionRecord(text: text))
        #expect(!record.running && !record.orderly)
        #expect(record.end == "guest-panic" && record.status == 6 && record.processID == 46851)
        #expect(record.started == Date(timeIntervalSince1970: 1_794_114_290))
        #expect(record.ended == Date(timeIntervalSince1970: 1_794_114_295))
        #expect(record.message == "sysrq triggered crash")
        #expect(record.kernelOutput?.hasPrefix("[    0.566837] Kernel panic") == true)
        #expect(record.summary == "The previous session ended in a guest kernel panic: sysrq triggered crash.")
    }

    @Test func aRecordStillRunningWasEndedFromOutside() throws {
        let record = try #require(SessionRecord(
            text: "uml-session 1\nstate running\nend running\nstatus 0\npid 7\nstarted 1794114339\n"))
        #expect(record.running && !record.orderly && record.ended == nil && record.kernelOutput == nil)
        #expect(record.summary.hasPrefix("The previous session was still running"))
    }

    @Test func anOrderlyEndIsOrderly() throws {
        let record = try #require(SessionRecord(
            text: "uml-session 1\nstate ended\nend powered-off\nstatus 0\npid 9\nstarted 1\nended 2\n"))
        #expect(record.orderly && record.message == nil)
    }

    @Test func otherTextIsNoRecord() {
        #expect(SessionRecord(text: "") == nil)
        #expect(SessionRecord(text: "uml-session 2\nstate ended\nend powered-off\n") == nil)
        #expect(SessionRecord(text: "uml-session 1\nstatus 0\n") == nil)
        #expect(SessionRecord.read(at: URL(fileURLWithPath: "/nonexistent/session.txt")) == nil)
    }
}
