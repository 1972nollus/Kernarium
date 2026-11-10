// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
import UIKit
@testable import Andarium

/// The bundled release notes and how a launch announces them: the person's
/// own launch gets the ribbon and keeps its record, any other shows no
/// ribbon and keeps none. Each test keeps into a defaults database of its own.
@MainActor
struct WhatsNewTests {
    private func withDefaults(_ body: (UserDefaults) throws -> Void) rethrows {
        let name = "andarium-tests-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }
        try body(defaults)
    }

    private func host(maximumMiB: Int) -> HostDevice {
        HostDevice(
            name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [maximumMiB]),
            freeStorageBytes: 40_000_000_000)
    }

    private func model(on host: HostDevice) -> AppModel {
        AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(
                version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1.0 (6)"),
            capabilities: .device, createRequirementBytes: 1)
    }

    private func entry(announces: Bool, build: Int? = 6, defaults: UserDefaults) -> AppEntry {
        var entry = AppEntry(overrides: LaunchOverrides())
        entry.announcesWhatsNew = announces
        entry.build = build
        entry.defaults = defaults
        return entry
    }

    // MARK: The bundled notes

    @Test func theBundledNotesDecodeAndStartWithBuildSix() throws {
        let url = try #require(Bundle.main.url(forResource: "WhatsNew", withExtension: "json"))
        let releases = try WhatsNewCatalog.decode(Data(contentsOf: url))
        #expect(releases.map(\.build) == [6])
        #expect(WhatsNewCatalog.load() == releases)
        let entries = try #require(releases.first?.entries)
        #expect(entries.map(\.id) == ["file-browser", "more-memory", "graphics-memory"])
        #expect(entries.first?.title == "File browser")
    }

    @Test func everyEntryHasWordsASymbolAndARequirementTheCodeKnows() throws {
        for release in WhatsNewCatalog.load() {
            #expect(!release.version.isEmpty)
            for entry in release.entries {
                #expect(!entry.title.isEmpty && !entry.text.isEmpty, "\(entry.id)")
                #expect(UIImage(systemName: entry.symbol) != nil, "\(entry.id): \(entry.symbol)")
                // A requirement the code does not know fails to decode, so the
                // entry would not be here; the memory one is the notes' own.
                if let requirement = entry.requirement { #expect(requirement.memoryAboveMiB != nil) }
            }
        }
        let memory = try #require(WhatsNewCatalog.load().flatMap(\.entries).first { $0.id == "more-memory" })
        #expect(memory.requirement == WhatsNewRequirement(memoryAboveMiB: 4096))
    }

    // MARK: The person's launch

    @Test func anUpdateFromBuildFiveAnnouncesTheNotesAndKeepsTheRecord() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 5))
            let host = host(maximumMiB: 6144)
            let model = model(on: host)
            entry(announces: true, defaults: defaults).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
            #expect(model.whatsNew.showsRibbon)
            #expect(model.whatsNew.pendingEntries.map(\.id) == ["file-browser", "more-memory", "graphics-memory"])
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 5, ribbonBuild: 6, ribbonLaunches: 1))
            #expect(model.whatsNew.releases.map(\.build) == [6])
        }
    }

    @Test func anInstallWithAGuestAndNoRecordIsAnUpdateFromBuildFive() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
            #expect(model.whatsNew.showsRibbon)
        }
    }

    @Test func aDeviceThatOffersNoMoreThanFourGiBIsNotToldAboutMoreMemory() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 4096))
            entry(announces: true, defaults: defaults).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
            #expect(model.whatsNew.pendingEntries.map(\.id) == ["file-browser", "graphics-memory"])
            // Info lists the notes as written.
            #expect(model.whatsNew.releases.first?.entries.count == 3)
        }
    }

    @Test func aNewInstallAnnouncesNothingAndRecordsThisBuild() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announceWhatsNew(on: model, hasGuest: false, vCPUs: 6)
            #expect(!model.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 6))
        }
    }

    @Test func buildFiveAndEarlierAnnounceNothing() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 4))
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, build: 5, defaults: defaults).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
            #expect(!model.whatsNew.showsRibbon)
        }
    }

    @Test func aBuildWithoutANumberAnnouncesNothingAndKeepsNothing() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, build: nil, defaults: defaults).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
            #expect(!model.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == nil)
        }
    }

    @Test func readingTheNotesAndPuttingTheRibbonAwayAreKept() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 5))
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
            model.whatsNew.read()
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 6))
            #expect(model.whatsNew.showsRibbon)
            model.whatsNew.putAway()
            #expect(!model.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 6))

            // The next launch of the same build offers nothing.
            let next = self.model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announceWhatsNew(on: next, hasGuest: true, vCPUs: 6)
            #expect(!next.whatsNew.showsRibbon)
        }
    }

    // MARK: A harness's launch

    @Test func aLaunchThatIsNotThePersonsShowsNoRibbonAndKeepsNoRecord() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 5))
            let model = model(on: host(maximumMiB: 6144))
            let launch = entry(announces: false, defaults: defaults)
            launch.announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
            #expect(!model.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 5))
            model.whatsNew.read()
            model.whatsNew.putAway()
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 5))
            // Info still lists the notes.
            #expect(model.whatsNew.releases.map(\.build) == [6])

            // The record of an install that has none stays none.
            withDefaults { fresh in
                let model = self.model(on: host(maximumMiB: 6144))
                self.entry(announces: false, defaults: fresh).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
                #expect(WhatsNewStore(defaults: fresh).record == nil)
            }
        }
    }

    #if !APP_STORE
    @Test func onlyALaunchWithNoSettingAndOutsideATestAnnounces() {
        func announces(_ environment: [String: String]) -> Bool {
            AppEntry.isPersonsLaunch(
                overrides: LaunchOverrides(environment: environment, processorCount: 6), environment: environment)
        }
        #expect(announces([:]))
        #expect(announces(["HOME": "/"]))
        #expect(!announces(["ANDARIUM_START": "normal"]))
        #expect(!announces(["ANDARIUM_GUEST_INSTANCE": "x"]))
        #expect(!announces(["KERNARIUM_VCPU_COUNT": "2"]))
        #expect(!announces(["ANDARIUM_STATS": "0"]))
        #expect(!announces(["ANDARIUM_JIT": "0"]))
        #expect(!announces(["XCTestConfigurationFilePath": "/tmp/x"]))
    }

    /// The process's own model is the real `makeModel`'s: Info lists the notes
    /// and the test run's launch announces none. (A test does not build a
    /// model of its own here: a `LiveRuntime` that is released takes Core
    /// Motion's device down with it, which crashes a Mac.)
    @Test func theTestRunsOwnLaunchIsAHarnessLaunch() {
        #expect(!AppEntry.fromArguments().announcesWhatsNew)
        let model = AndariumProcess.shared.model
        #expect(!model.whatsNew.showsRibbon)
        #expect(model.whatsNew.releases.map(\.build) == [6])
    }
    #endif
}
