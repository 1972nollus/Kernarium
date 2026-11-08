// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// The graphics memory reading and the activity manager's process list on a
/// real guest. Runs alone with ANDARIUM_GRAPHICS_RECLAIM_TESTS=1 and
/// ANDARIUM_GUEST_INSTANCE=graphics-reclaim, on a Mac (Designed for iPad) or
/// an iPad, with an isolated guest. The test installs nothing and ends no
/// app: it reads the budget through the runtime and parses what `dumpsys
/// activity lru` prints, which the policy's exactness rests on.
@MainActor
struct GraphicsReclaimGuestTests {
    @Test(.timeLimit(.minutes(25)))
    func theBudgetAndTheProcessListAreReadFromARunningGuest() async throws {
        guard ProcessInfo.processInfo.environment["ANDARIUM_GRAPHICS_RECLAIM_TESTS"] == "1" else { return }
        try #require(ProcessInfo.processInfo.environment["ANDARIUM_GUEST_INSTANCE"] == "graphics-reclaim")
        try await withAndroidGuest { model, operations in
            let driver = try #require(model.driver)

            let reading = try #require(await driver.readGraphicsMemory(minimumIdle: .zero, minimumBytes: 0))
            print(
                "GRAPHICS_RECLAIM: budget \(reading.budgetBytes >> 20) MiB, usage \(reading.usageBytes >> 20) MiB, "
                    + "contexts \(reading.contextBytes >> 20) MiB, holders \(reading.holders.map { "\($0.pid):\($0.name):\($0.bytes >> 20)" })")
            #expect(reading.budgetBytes > 0, "the host gave the guest no graphics budget")
            #expect(reading.usageBytes > 0)
            #expect(reading.holders.count <= 8)
            #expect(reading.holders.map(\.bytes) == reading.holders.map(\.bytes).sorted(by: >), "not largest first")
            #expect(reading.holders.allSatisfy { $0.name.utf8.count <= 15 })

            // The filters hide what they exclude and nothing else.
            let large = try #require(
                await driver.readGraphicsMemory(minimumIdle: .zero, minimumBytes: reading.budgetBytes))
            #expect(large.holders.isEmpty, "a holder larger than the budget was listed")
            #expect(large.budgetBytes == reading.budgetBytes)
            let quiet = try #require(await driver.readGraphicsMemory(minimumIdle: .seconds(86400), minimumBytes: 0))
            #expect(quiet.holders.isEmpty, "a holder quiet for a day on a guest that has just booted")

            // The process list the policy reads parses on the real guest.
            let list = try await operations.shell(AndroidProcessList.command, limit: .seconds(60))
            #expect(list.exitCode == 0)
            let processes = AndroidProcessList.parse(list.text)
            print("GRAPHICS_RECLAIM: \(processes.count) processes; standings "
                + "\(processes.filter { $0.standing == .cached }.count) cached, "
                + "\(processes.filter { $0.standing == .previous }.count) previous")
            #expect(processes.count > 10, "the process list did not parse: \(list.text.prefix(400))")
            #expect(processes.contains { $0.name == "system" }, "system_server is not in the parsed list")
            #expect(processes.contains { $0.standing == .kept })
            #expect(processes.allSatisfy { $0.pid > 0 && !$0.label.isEmpty && !$0.state.isEmpty })

            // Without the kernel's process naming no holder has a pid, and
            // nothing is ended.
            if reading.holders.allSatisfy({ $0.pid == 0 }) {
                var policy = GraphicsReclaim()
                let now = ContinuousClock.now
                let first = policy.observe(reading, at: now)
                let second = policy.observe(reading, at: now.advanced(by: .seconds(2)))
                #expect(!first && !second)
            }
        }
    }
}
