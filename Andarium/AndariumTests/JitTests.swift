// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// A defaults database of its own for one test, removed after it.
private func withDefaults<Result>(_ body: (UserDefaults) throws -> Result) rethrows -> Result {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("andarium-jit-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let name = directory.appendingPathComponent("defaults").path
    let defaults = UserDefaults(suiteName: name)!
    defer {
        defaults.removePersistentDomain(forName: name)
        try? FileManager.default.removeItem(at: directory)
    }
    return try body(defaults)
}

@MainActor
struct JitPreferenceTests {
    @Test func aFreshInstallIsOff() {
        withDefaults { defaults in
            let preference = JitPreference(defaults: defaults)
            #expect(!preference.isOn)
            let start = preference.launch(stated: nil, hasOtherSettings: false)
            #expect(!start.isOn)
            #expect(start.keepsChanges)
        }
    }

    @Test func theLastValueIsKeptAcrossLaunches() {
        withDefaults { defaults in
            JitPreference(defaults: defaults).keep(true)
            #expect(JitPreference(defaults: defaults).isOn)
            let on = JitPreference(defaults: defaults).launch(stated: nil, hasOtherSettings: false)
            #expect(on.isOn && on.keepsChanges)
            JitPreference(defaults: defaults).keep(false)
            #expect(!JitPreference(defaults: defaults).launch(stated: nil, hasOtherSettings: false).isOn)
        }
    }

    @Test func aLaunchThatStatesTheValueForcesItAndKeepsNothing() {
        withDefaults { defaults in
            let preference = JitPreference(defaults: defaults)
            preference.keep(true)
            let off = preference.launch(stated: false, hasOtherSettings: true)
            #expect(!off.isOn && !off.keepsChanges)
            preference.keep(false)
            let on = preference.launch(stated: true, hasOtherSettings: false)
            #expect(on.isOn && !on.keepsChanges)
            #expect(!preference.isOn)
        }
    }

    @Test func aLaunchWithOtherSettingsStartsOffWhateverWasKept() {
        withDefaults { defaults in
            let preference = JitPreference(defaults: defaults)
            preference.keep(true)
            let start = preference.launch(stated: nil, hasOtherSettings: true)
            #expect(!start.isOn && !start.keepsChanges)
            #expect(preference.isOn)
        }
    }

    @Test func theEnvironmentStatesTheValueOrNothing() {
        #expect(JitPreference.stated(in: ["ANDARIUM_JIT": "1"]) == true)
        #expect(JitPreference.stated(in: ["ANDARIUM_JIT": "0"]) == false)
        #expect(JitPreference.stated(in: ["ANDARIUM_JIT": "yes"]) == nil)
        #expect(JitPreference.stated(in: [:]) == nil)
    }

    @Test func itIsAValueOfItsOwn() {
        #expect(JitPreference.key != StatsPreference.key)
        #expect(JitPreference.key != DisplayOptions.storageKey)
    }
}

@MainActor
struct JitAnswerTests {
    @Test func theRuntimeValuesMapToTheirAnswers() {
        #expect(JitAnswer(runtimeValue: 0) == .ready)
        #expect(JitAnswer(runtimeValue: 1) == .unsupported)
        #expect(JitAnswer(runtimeValue: 2) == .noDebugger)
        #expect(JitAnswer(runtimeValue: 3) == .failed)
        #expect(JitAnswer(runtimeValue: 4) == .otherSettings)
        #expect(JitAnswer(runtimeValue: 5) == .disabled)
        // An answer a later runtime adds is a failure, never a ready JIT.
        #expect(JitAnswer(runtimeValue: 6) == .failed)
        #expect(JitAnswer(runtimeValue: -1) == .failed)
    }

    @Test func onlyAFallBackHasASentence() {
        #expect(JitAnswer.ready.fallbackSentence == nil)
        #expect(JitAnswer.disabled.fallbackSentence == nil)
        #expect(JitAnswer.noDebugger.fallbackSentence == "Started without JIT: StikDebug is not connected.")
        for answer in [JitAnswer.failed, .otherSettings, .unsupported] {
            let sentence = answer.fallbackSentence
            #expect(sentence?.hasPrefix("Started without JIT: ") == true)
        }
    }
}

@MainActor
struct JitStatusTests {
    private let guest = JitShape(memoryMiB: 4096, processorCount: 4)

    @Test func withoutADebuggerItSaysHowToGetOne() {
        let status = JitStatus(debuggerAttached: false, guest: guest)
        #expect(!status.usable)
        #expect(status.footer == "To use JIT, open Andarium from StikDebug.")
    }

    @Test func withADebuggerItIsReady() {
        let status = JitStatus(debuggerAttached: true, guest: guest)
        #expect(status.usable)
        #expect(status.footer == "Ready. Applies to the next start.")
    }

    @Test func preparedMemoryIsReadyAfterTheDebuggerLeft() {
        let status = JitStatus(debuggerAttached: false, prepared: guest, guest: guest)
        #expect(status.usable)
        #expect(status.footer == JitStatus.ready)
    }

    @Test func aGuestChangedAfterPreparingNeedsANewProcess() {
        let other = JitShape(memoryMiB: 2048, processorCount: 4)
        for attached in [false, true] {
            let status = JitStatus(debuggerAttached: attached, prepared: guest, guest: other)
            #expect(!status.usable)
            #expect(status.footer == JitStatus.changed)
        }
        let cores = JitShape(memoryMiB: 4096, processorCount: 6)
        #expect(JitStatus(debuggerAttached: false, prepared: guest, guest: cores).footer == JitStatus.changed)
    }

    @Test func aStartThatFellBackSaysSoUntilTheNextStart() {
        var status = JitStatus(debuggerAttached: false, guest: guest, lastStart: .noDebugger)
        #expect(status.footer == "Started without JIT: StikDebug is not connected.")
        status.lastStart = nil
        #expect(status.footer == JitStatus.needsStikDebug)
        // An answer that is no fall-back leaves the state's own words.
        status.lastStart = .ready
        #expect(status.footer == JitStatus.needsStikDebug)
    }
}
