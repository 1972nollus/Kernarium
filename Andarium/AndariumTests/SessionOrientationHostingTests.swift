// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

@MainActor
@Suite(.serialized)
struct SessionOrientationHostingTests {
    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    @Test func theSceneRootLocksOnlyTheGuestSession() async throws {
        let entry = AppEntry.fromArguments()
        let model = entry.makeModel()
        try #require(await eventually {
            UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
                .contains { $0.windows.contains { $0.isKeyWindow } }
        })
        let scene = try #require(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
        let keyWindow = scene.windows.first { $0.isKeyWindow }
        let window = try #require(keyWindow)
        let original = window.rootViewController
        defer { window.rootViewController = original }
        let controller = AndariumHostingController(rootView: Color.black, model: model)
        window.rootViewController = controller
        let phone = UIDevice.current.userInterfaceIdiom == .phone
        #expect(controller.supportedInterfaceOrientations == (phone ? .portrait : .all))
        #expect(!controller.prefersInterfaceOrientationLocked)
        model.phase = .running(.normal)
        let size = scene.effectiveGeometry.coordinateSpace.bounds.size
        let screen = scene.screen.bounds.size
        let fullScreen = abs(size.width - screen.width) < 0.5 && abs(size.height - screen.height) < 0.5
        if !phone && fullScreen {
            #expect(await eventually { controller.prefersInterfaceOrientationLocked })
            #expect(await eventually { scene.effectiveGeometry.isInterfaceOrientationLocked })
            #expect(await eventually { !model.hostDrivesGuestRotation })
        } else {
            #expect(!controller.prefersInterfaceOrientationLocked)
            #expect(await eventually { model.hostDrivesGuestRotation == !phone })
        }
        model.phase = .home
        #expect(await eventually { !controller.prefersInterfaceOrientationLocked && !model.hostDrivesGuestRotation })
    }

    @Test func theSceneRootTellsTheDriverWhenTheGeometryChanges() async throws {
        let entry = AppEntry.fromArguments()
        let model = entry.makeModel()
        let driver = StubDriver(plan: nil)
        model.driver = driver
        try #require(await eventually {
            UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
                .contains { $0.windows.contains { $0.isKeyWindow } }
        })
        let scene = try #require(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
        let window = try #require(scene.windows.first { $0.isKeyWindow })
        let original = window.rootViewController
        defer { window.rootViewController = original }
        let controller = AndariumHostingController(rootView: Color.black, model: model)
        window.rootViewController = controller
        #expect(await eventually { driver.geometryChanges > 0 })
        // Let the first layouts settle, then report a storm of geometry
        // updates within one turn: the driver hears of it, and not once for
        // each of them.
        try await Task.sleep(for: .milliseconds(500))
        let before = driver.geometryChanges
        for _ in 0..<50 { controller.sceneGeometryDidChange() }
        #expect(await eventually { driver.geometryChanges > before })
        try await Task.sleep(for: .milliseconds(200))
        #expect(driver.geometryChanges - before <= 5)
    }

    @Test func theBundleAllowsOnlyPortraitOnIPhone() throws {
        // Read the declarations themselves: Bundle's device-qualified lookup
        // resolves orientation keys for the iPad running this test.
        let data = try Data(contentsOf: Bundle.main.bundleURL.appendingPathComponent("Info.plist"))
        let info = try #require(try PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any])
        let orientations = info["UISupportedInterfaceOrientations~iphone"] ?? info["UISupportedInterfaceOrientations"]
        #expect(orientations as? [String] == ["UIInterfaceOrientationPortrait"])
        #expect((info["UISupportedInterfaceOrientations~ipad"] as? [String])?.count == 4)
    }
}
