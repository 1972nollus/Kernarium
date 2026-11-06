// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// Microphone asks iOS when it is turned on, and Gyroscope asks for nothing:
/// raw accelerometer and gyroscope updates need no permission on iOS.
@MainActor
struct CapabilitiesFlowTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func model(driver: Bool = true) -> (AppModel, StubDriver) {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1, systemBytes: 1
        )
        let stub = StubDriver(plan: .normal)
        if driver { model.driver = stub }
        return (model, stub)
    }

    @Test func turningItOnAsksIOSAtOnceAndRefusalTakesTheChoiceBack() {
        let (model, driver) = model()
        #expect(model.microphoneState == .notAsked)
        model.setMicrophone(true)
        // The runtime driver asks iOS now.
        #expect(driver.microphoneCalls == [true])
        #expect(model.guest?.microphone == true)
        // iOS says no: the switch goes off and nothing is stored on.
        model.microphoneAccessChanged(.denied)
        #expect(model.microphoneState == .denied)
        #expect(model.guest?.microphone == false)
        #expect(!model.microphoneShownOn)
        #expect(driver.savedGuest?.microphone == false)
    }

    @Test func anAllowedRequestKeepsTheSwitchOn() {
        let (model, _) = model()
        model.setMicrophone(true)
        model.microphoneAccessChanged(.granted)
        #expect(model.microphoneShownOn)
        #expect(model.permissionPrompt == nil)
    }

    @Test func afterARefusalTheSwitchExplainsAndOffersSettings() {
        let (model, driver) = model()
        model.microphoneAccessChanged(.denied)
        var opened = 0
        model.openSettings = { opened += 1 }
        model.setMicrophone(true)
        // Nothing is asked and nothing is turned on: the person is told.
        #expect(model.permissionPrompt == .microphone)
        #expect(model.guest?.microphone == false)
        #expect(driver.microphoneCalls.isEmpty)
        #expect(opened == 0)
        model.openMicrophoneSettings()
        #expect(opened == 1)
        #expect(model.microphoneAwaitsSettings)
    }

    @Test func allowingItInSettingsTurnsTheSwitchTheyTappedOn() {
        let (model, _) = model()
        model.microphoneAccessChanged(.denied)
        model.setMicrophone(true)
        model.openMicrophoneSettings()
        // Back in Andarium with access: the switch is on.
        model.microphoneAccessChanged(.granted)
        #expect(model.microphoneShownOn)
        #expect(!model.microphoneAwaitsSettings)
    }

    @Test func comingBackWithoutAccessLeavesItOffAndForgetsTheRequest() {
        let (model, _) = model()
        model.microphoneAccessChanged(.denied)
        model.setMicrophone(true)
        model.openMicrophoneSettings()
        model.microphoneAccessChanged(.denied)
        #expect(!model.microphoneShownOn)
        #expect(!model.microphoneAwaitsSettings)
        // Allowing it later, unprompted, does not turn it on by itself.
        model.microphoneAccessChanged(.granted)
        #expect(!model.microphoneShownOn)
    }

    @Test func accessTakenAwayInSettingsShowsItOffAndStoresItOff() {
        let (model, driver) = model()
        model.setMicrophone(true)
        model.microphoneAccessChanged(.granted)
        #expect(model.microphoneShownOn)
        model.microphoneAccessChanged(.denied)
        #expect(!model.microphoneShownOn)
        #expect(model.guest?.microphone == false)
        #expect(driver.savedGuest?.microphone == false)
    }

    @Test func turningItOffTellsTheGuest() {
        let (model, driver) = model()
        model.microphoneAccessChanged(.granted)
        model.setMicrophone(true)
        model.setMicrophone(false)
        #expect(!model.microphoneShownOn)
        #expect(driver.microphoneCalls == [true, false])
    }

    @Test func thePreviewHasNoPromptAndTakesAccessAsGiven() {
        let (model, _) = model(driver: false)
        model.setMicrophone(true)
        #expect(model.microphoneState == .granted)
        #expect(model.microphoneShownOn)
    }

    @Test func gyroscopeAsksForNothing() {
        let (model, driver) = model()
        model.setMotionSensors(true)
        #expect(model.guest?.motionSensors == true)
        #expect(driver.motionSensors)
        #expect(model.permissionPrompt == nil)
        #expect(model.microphoneState == .notAsked)
        model.setMotionSensors(false)
        #expect(model.guest?.motionSensors == false)
    }

    @Test func gyroscopeStaysOffOnADeviceWithNoSensors() {
        let (model, driver) = model()
        model.motionSensorsAvailable = false
        model.setMotionSensors(true)
        #expect(model.guest?.motionSensors == false)
        #expect(!driver.motionSensors)
    }
}
