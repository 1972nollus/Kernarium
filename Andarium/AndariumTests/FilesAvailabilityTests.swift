// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// The file browser waits for Android's storage: Android reports its boot
/// done before `/storage/emulated/0` is mounted, and until then an empty root
/// and a refused write would be false.
@MainActor
struct FilesAvailabilityTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )
    private let system = SystemInfo(
        version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1")

    private var download: String { GuestPath.child("Download", of: GuestPath.root) }

    /// A running guest whose browser stands in `Download`, listed.
    private func running() async -> (AppModel, StubDriver, FileBrowserModel) {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.refreshPlan()
        let backend = FileBrowserModelTests.ScriptedBackend()
        let browser = FileBrowserModel(backend: backend, center: NotificationCenter())
        model.fileBrowser = browser
        model.start(.normal)
        model.finishBoot(.normal)
        browser.route = [FolderRoute(path: GuestPath.root), FolderRoute(path: download)]
        await browser.load(download)
        return (model, driver, browser)
    }

    @Test func theBrowserWaitsUntilAndroidSaysItsStorageIsUp() async {
        let (model, driver, _) = await running()
        model.session.usbDebugging = true
        // Not read yet, and read as starting: not available.
        #expect(!model.filesAvailable)
        driver.storage = false
        await model.refreshStorage()
        #expect(model.session.storageReady == false && !model.filesAvailable)
        // A reading that fails changes nothing.
        driver.storage = nil
        await model.refreshStorage()
        #expect(model.session.storageReady == false)
        // Once it is up the browser is available, with no reopening.
        driver.storage = true
        await model.refreshStorage()
        #expect(model.session.storageReady == true && model.filesAvailable)
    }

    @Test func usbDebuggingOffStillComesFirst() async {
        let (model, driver, _) = await running()
        driver.storage = true
        await model.refreshStorage()
        model.session.usbDebugging = false
        #expect(!model.filesAvailable)
    }

    @Test func onceUpStorageIsNotAskedAgainAndTheRunEndingForgetsIt() async {
        let (model, driver, _) = await running()
        model.session.usbDebugging = true
        driver.storage = true
        await model.refreshStorage()
        driver.storage = false
        await model.refreshStorage()
        #expect(model.session.storageReady == true, "up stays up for the run")
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
        #expect(model.session.storageReady == nil)
    }

    @Test func nothingIsImportedOrExportedWhileItIsStarting() async throws {
        let (model, driver, browser) = await running()
        model.session.usbDebugging = true
        driver.storage = false
        await model.refreshStorage()
        let file = GuestFile(path: GuestPath.child("a.txt", of: download), isFolder: false, size: 1, modified: .now)
        browser.export([file])
        browser.received([FilePick(name: "b.txt", size: 1)])
        #expect(browser.transfer == nil && browser.conflict == nil)
        #expect(!browser.canImport(into: download))
        // The same browser takes work as soon as the storage is up.
        driver.storage = true
        await model.refreshStorage()
        #expect(browser.canImport(into: download))
        browser.received([FilePick(name: "b.txt", size: 1)])
        #expect(browser.transfer != nil)
        browser.closeTransfer()
    }

    @Test func thePreviewHasNoGuestToAskAndSaysYesUnlessAScreenSaysNo() {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .preview, createRequirementBytes: 1)
        #expect(model.filesAvailable)
        model.session.storageReady = false
        #expect(!model.filesAvailable)
    }
}
