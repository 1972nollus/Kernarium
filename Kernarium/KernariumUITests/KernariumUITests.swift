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
        XCTAssertTrue(
            app.descendants(matching: .any)["kernelPageSizePicker"].exists
        )
        XCTAssertTrue(
            app.descendants(matching: .any)["rootfsPicker"].exists
        )
        XCTAssertTrue(app.buttons["settingsSaveButton"].exists)
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
