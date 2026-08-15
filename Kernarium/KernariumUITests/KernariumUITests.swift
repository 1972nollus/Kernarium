//
//  KernariumUITests.swift
//  KernariumUITests
//
//  Created by Yi Xie on 2026/08/12.
//

import XCTest

final class KernariumUITests: XCTestCase {

    override func setUpWithError() throws {
        // Put setup code here. This method is called before the invocation of each test method in the class.

        // In UI tests it is usually best to stop immediately when a failure occurs.
        continueAfterFailure = false

        // In UI tests it’s important to set the initial state - such as interface orientation - required for your tests before they run. The setUp method is a good place to do this.
    }

    override func tearDownWithError() throws {
        // Put teardown code here. This method is called after the invocation of each test method in the class.
    }

    @MainActor
    func testSettingsSheetShowsLaunchConfigurationControls() throws {
        let app = testApplication()
        app.launch()

        let settingsButton = app.buttons["settingsButton"]
        XCTAssertTrue(settingsButton.waitForExistence(timeout: 10))
        settingsButton.tap()

        XCTAssertTrue(
            app.steppers["vCPUCountStepper"].waitForExistence(timeout: 5)
        )
        XCTAssertTrue(app.steppers["guestMemoryStepper"].exists)
        XCTAssertTrue(app.steppers["overlaySizeStepper"].exists)
        XCTAssertTrue(app.switches["retinaModeToggle"].exists)
        XCTAssertTrue(
            app.descendants(matching: .any)["kernelPageSizePicker"].exists
        )
        XCTAssertTrue(
            app.descendants(matching: .any)["rootfsPicker"].exists
        )
        XCTAssertTrue(app.buttons["settingsSaveButton"].exists)
    }

    @MainActor
    func testSpecialKeyTrayExpandsAndCollapses() throws {
        let app = testApplication()
        app.launch()

        let trayButton = app.buttons["specialKeysButton"]
        XCTAssertTrue(trayButton.waitForExistence(timeout: 10))
        XCTAssertEqual(trayButton.frame.width, 32, accuracy: 1)
        XCTAssertEqual(trayButton.frame.height, 32, accuracy: 1)
        XCTAssertFalse(app.buttons["guestPowerButton"].exists)

        trayButton.tap()
        for identifier in [
            "guestPowerButton",
            "guestVolumeUpButton",
            "guestVolumeDownButton",
            "guestEscapeButton",
            "guestTabButton",
            "guestHomeButton",
        ] {
            XCTAssertTrue(app.buttons[identifier].waitForExistence(timeout: 2))
        }

        trayButton.tap()
        XCTAssertTrue(
            app.buttons["guestPowerButton"].waitForNonExistence(timeout: 2)
        )
    }

    @MainActor
    func testDisplayModeDefaultsToFramebufferAndToggles() throws {
        let app = testApplication()
        app.launch()

        let displayModeButton = app.buttons["displayModeButton"]
        XCTAssertTrue(displayModeButton.waitForExistence(timeout: 10))
        XCTAssertEqual(displayModeButton.label, "Show Serial Console")

        displayModeButton.tap()
        XCTAssertEqual(displayModeButton.label, "Show Framebuffer")

        displayModeButton.tap()
        XCTAssertEqual(displayModeButton.label, "Show Serial Console")
    }

    @MainActor
    func testLaunchPerformance() throws {
        // This measures how long it takes to launch your application.
        measure(metrics: [XCTApplicationLaunchMetric()]) {
            testApplication().launch()
        }
    }

    private func testApplication() -> XCUIApplication {
        let app = XCUIApplication()
        // UI tests exercise the app shell, not a multi-gigabyte guest boot.
        app.launchEnvironment["KERNARIUM_ROOTFS"] =
            "kernarium-ui-tests-missing.erofs"
        return app
    }
}
