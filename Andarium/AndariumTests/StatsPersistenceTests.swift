// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// Settings' "Show on screen" is kept across launches, and a launch that
/// states the value for its own run leaves what the person set alone.
@MainActor
struct StatsPersistenceTests {
    private func model() -> AppModel {
        AppModel(
            guest: nil, host: AppEntry.hostDevice(),
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
    }

    @Test func theValueAFreshModelKeepsIsOff() {
        let model = model()
        #expect(!model.statsStatedByLaunch)
        #expect(model.showStatsToKeep == false)
        model.session.showStats = true
        #expect(model.showStatsToKeep == true)
    }

    @Test func aValueTheLaunchStatedIsNotKept() {
        let model = model()
        model.statsStatedByLaunch = true
        model.session.showStats = true
        #expect(model.showStatsToKeep == nil)
    }

    @Test func theRuntimeWritesTheTogglesTheLaunchDidNotState() async throws {
        let defaults = UserDefaults.standard
        let saved = defaults.object(forKey: StatsPreference.key)
        defer {
            if let saved { defaults.set(saved, forKey: StatsPreference.key) } else { defaults.removeObject(forKey: StatsPreference.key) }
        }
        defaults.removeObject(forKey: StatsPreference.key)

        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let systemDirectory = try #require(Bundle.main.resourceURL).appendingPathComponent(SystemFolder.name)
        let release = try SystemFolder.release(in: systemDirectory)
        let storage = TwoDiskStorage(
            locations: GuestLocations(directory: directory), release: release.identity, releaseDirectory: systemDirectory
        )
        let model = model()
        let runtime = LiveRuntime(model: model, storage: storage, marker: PauseMarker(url: directory.appendingPathComponent("pause.json")))
        defer { withExtendedLifetime(runtime) {} }

        func settles(_ expected: Bool) async throws -> Bool {
            let deadline = ContinuousClock.now + .seconds(2)
            while StatsPreference().isOn != expected, ContinuousClock.now < deadline {
                try await Task.sleep(for: .milliseconds(20))
            }
            return StatsPreference().isOn == expected
        }
        model.session.showStats = true
        #expect(try await settles(true))
        model.session.showStats = false
        #expect(try await settles(false))

        // A launch that states the value is the harness's.
        model.session.showStats = true
        #expect(try await settles(true))
        model.statsStatedByLaunch = true
        model.session.showStats = false
        try await Task.sleep(for: .milliseconds(300))
        #expect(StatsPreference().isOn)
    }
}
