// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

/// A defaults database of its own for one test, a file in a directory that
/// is removed after it, so the test leaves nothing in the user's preferences.
private func withDefaults<Result>(_ body: (UserDefaults) throws -> Result) rethrows -> Result {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("andarium-stats-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    // An absolute path as the suite name keeps the preferences file there.
    let name = directory.appendingPathComponent("defaults").path
    let defaults = UserDefaults(suiteName: name)!
    defer {
        defaults.removePersistentDomain(forName: name)
        try? FileManager.default.removeItem(at: directory)
    }
    return try body(defaults)
}

struct StatsPreferenceTests {
    @Test func aFreshInstallIsOff() {
        withDefaults { defaults in
            #expect(!StatsPreference(defaults: defaults).isOn)
            let launch = StatsPreference(defaults: defaults).launch(stated: nil)
            #expect(!launch.showOnScreen)
            #expect(launch.keepsChanges)
        }
    }

    @Test func theLastValueIsKeptAcrossLaunches() {
        withDefaults { defaults in
            StatsPreference(defaults: defaults).keep(true)
            // Another launch reads the same database with a new value.
            #expect(StatsPreference(defaults: defaults).isOn)
            #expect(StatsPreference(defaults: defaults).launch(stated: nil).showOnScreen)
            StatsPreference(defaults: defaults).keep(false)
            #expect(!StatsPreference(defaults: defaults).isOn)
            #expect(!StatsPreference(defaults: defaults).launch(stated: nil).showOnScreen)
        }
    }

    @Test func aLaunchThatStatesTheValueForcesItAndKeepsNothing() {
        withDefaults { defaults in
            let preference = StatsPreference(defaults: defaults)
            preference.keep(true)
            let off = preference.launch(stated: false)
            #expect(!off.showOnScreen && !off.keepsChanges)
            preference.keep(false)
            let on = preference.launch(stated: true)
            #expect(on.showOnScreen && !on.keepsChanges)
            // Stating the value never touched what was kept.
            #expect(!preference.isOn)
        }
    }

    @Test func storedDisplayOptionsFromBeforeSurviveAndAreNotRewritten() throws {
        try withDefaults { defaults in
            // What an earlier version stored: the Retina choice, no statistics.
            let old = Data(#"{"retina":true,"chosen":true}"#.utf8)
            defaults.set(old, forKey: DisplayOptions.storageKey)
            let preference = StatsPreference(defaults: defaults)
            #expect(!preference.isOn)

            preference.keep(true)
            #expect(preference.isOn)
            let stored = try #require(defaults.data(forKey: DisplayOptions.storageKey))
            #expect(stored == old)
            #expect(try JSONDecoder().decode(DisplayOptions.self, from: stored) == DisplayOptions(chosenRetina: true))
        }
    }

    @Test func keepingTheDisplayOptionsLeavesTheStatisticsAlone() throws {
        try withDefaults { defaults in
            let preference = StatsPreference(defaults: defaults)
            preference.keep(true)
            defaults.set(try JSONEncoder().encode(DisplayOptions(chosenRetina: false)), forKey: DisplayOptions.storageKey)
            #expect(preference.isOn)
            #expect(StatsPreference.key != DisplayOptions.storageKey)
        }
    }
}
