import Darwin
import Foundation
import Testing
import UIKit
@testable import Kernarium

struct KernariumTests {
    private static let zstdFixture = Data([
        0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x68, 0xf4, 0x00,
        0x00, 0xb0, 0x4b, 0x45, 0x52, 0x4e, 0x41, 0x52,
        0x49, 0x55, 0x4d, 0x2d, 0x5a, 0x53, 0x54, 0x44,
        0x2d, 0x42, 0x45, 0x47, 0x49, 0x4e, 0x0a, 0x00,
        0x01, 0x00, 0xce, 0xff, 0x73, 0xd0, 0x04, 0x02,
        0x00, 0x10, 0x00, 0x02, 0x00, 0x10, 0x00, 0x02,
        0x00, 0x10, 0x00, 0x02, 0x00, 0x10, 0x00, 0x02,
        0x00, 0x10, 0x00, 0x02, 0x00, 0x10, 0x00, 0x02,
        0x00, 0x10, 0x00, 0x02, 0x00, 0x10, 0x00, 0x02,
        0x00, 0x10, 0x00, 0x02, 0x00, 0x10, 0x00, 0x02,
        0x00, 0x10, 0x00, 0x02, 0x00, 0x10, 0x00, 0x02,
        0x00, 0x10, 0x00, 0x02, 0x00, 0x10, 0x00, 0x02,
        0x00, 0x10, 0x00, 0x75, 0x00, 0x00, 0x20, 0x45,
        0x4e, 0x44, 0x0a, 0x02, 0x00, 0x18, 0x00, 0x80,
        0x74, 0xf0, 0x01, 0x20, 0xe7, 0x79, 0xb9, 0xf0,
    ])
    private static let zstdPrefix = Data("KERNARIUM-ZSTD-BEGIN\n".utf8)
    private static let zstdSuffix = Data("KERNARIUM-ZSTD-END\n".utf8)
    private static let zstdOutputSize = 2 * 1024 * 1024 +
        zstdPrefix.count + zstdSuffix.count

    private struct StubDiskCapacityManager: DiskCapacityManaging {
        func resizeAndroidImage(at _: URL, toGiB _: Int) throws -> Bool {
            true
        }
    }

    @Test
    func thermalStateReportHasStableMachineReadableFields() {
        #expect(ThermalStateReport.name(for: .nominal) == "nominal")
        #expect(ThermalStateReport.name(for: .fair) == "fair")
        #expect(ThermalStateReport.name(for: .serious) == "serious")
        #expect(ThermalStateReport.name(for: .critical) == "critical")
        #expect(
            ThermalStateReport.line(
                phase: .start,
                state: .nominal,
                uptime: 12.5
            ) == "Kernarium thermal state: phase=start state=nominal " +
                "uptime=12.500"
        )
    }

    @Test
    @MainActor
    func floatingControlsAndSpecialKeysHaveExpectedGeometryAndCodes() {
        #expect(FloatingControlMetrics.buttonSide == 32)
        #expect(GuestViewMode.defaultMode == .framebuffer)
        #expect(UIImage(systemName: "terminal") != nil)
        #expect(UIImage(systemName: "display") != nil)
        #expect(GuestSpecialKey.power.code == 116)
        #expect(GuestSpecialKey.volumeUp.code == 115)
        #expect(GuestSpecialKey.volumeDown.code == 114)
        #expect(GuestSpecialKey.escape.code == 1)
        #expect(GuestSpecialKey.tab.code == 15)
        #expect(GuestSpecialKey.home.code == 102)
        for key in GuestSpecialKey.allCases {
            #expect(UIImage(systemName: key.systemName) != nil)
        }
    }

    @Test
    @MainActor
    func touchCoordinatesStayAlignedWhenTheDisplayViewResizes() throws {
        let coordinates = GuestInputCoordinateSpace(
            size: CGSize(width: 440, height: 860)
        )
        let portrait = try #require(coordinates.location(
            CGPoint(x: 200, y: 400),
            in: CGSize(width: 400, height: 800)
        ))
        let landscape = try #require(coordinates.location(
            CGPoint(x: 500, y: 250),
            in: CGSize(width: 1000, height: 500)
        ))
        #expect(portrait.x == 220)
        #expect(portrait.y == 430)
        #expect(landscape.x == portrait.x)
        #expect(landscape.y == portrait.y)

        let lowerEdge = try #require(coordinates.location(
            CGPoint(x: 1000, y: 500),
            in: CGSize(width: 1000, height: 500)
        ))
        #expect(lowerEdge.x == 439)
        #expect(lowerEdge.y == 859)
        #expect(coordinates.location(
            CGPoint(x: CGFloat.nan, y: 1),
            in: CGSize(width: 1000, height: 500)
        ) == nil)

        let oversized = GuestInputCoordinateSpace.boundedDisplaySize(
            CGSize(width: 6016, height: 3384)
        )
        #expect(oversized.width == 4095)
        #expect(oversized.height == 3384)
    }

    @Test
    @MainActor
    func touchCoordinatesAccountForAspectFitLetterboxing() throws {
        let coordinates = GuestInputCoordinateSpace(
            size: CGSize(width: 400, height: 800)
        )
        let bounds = CGSize(width: 500, height: 250)
        let drawable = CGSize(width: 1000, height: 500)
        let square = CGSize(width: 800, height: 800)
        let viewport = try #require(GuestDisplayViewport.aspectFit(
            contentSize: square,
            in: drawable
        ))
        #expect(viewport == CGRect(x: 250, y: 0, width: 500, height: 500))

        #expect(coordinates.location(
            CGPoint(x: 100, y: 125),
            in: bounds,
            drawableSize: drawable,
            contentSize: square
        ) == nil)

        let center = try #require(coordinates.location(
            CGPoint(x: 250, y: 125),
            in: bounds,
            drawableSize: drawable,
            contentSize: square
        ))
        #expect(center.x == 200)
        #expect(center.y == 400)

        let clampedBar = try #require(coordinates.location(
            CGPoint(x: 100, y: 125),
            in: bounds,
            drawableSize: drawable,
            contentSize: square,
            clampToContent: true
        ))
        #expect(clampedBar.x == 0)
        #expect(clampedBar.y == 400)

        let wideModeCenter = try #require(coordinates.location(
            CGPoint(x: 250, y: 125),
            in: bounds,
            drawableSize: drawable,
            contentSize: CGSize(width: 1600, height: 900)
        ))
        #expect(wideModeCenter.x == center.x)
        #expect(wideModeCenter.y == center.y)
        #expect(GuestDisplayViewport.aspectFit(
            contentSize: .zero,
            in: drawable
        ) == nil)
    }

    @Test
    @MainActor
    func touchscreenRangeIsFixedAndIndependentOfTheViewSize() throws {
        let maximum = UInt32(KERNARIUM_RUNTIME_TOUCH_MAXIMUM)
        #expect(maximum == 32767)
        let coordinates = GuestInputCoordinateSpace.touchscreen
        #expect(coordinates.size == CGSize(width: 32768, height: 32768))

        // The same fraction of any view is the same coordinate, from the
        // launch size to a window grown well past it.
        for bounds in [
            CGSize(width: 440, height: 860),
            CGSize(width: 1133, height: 692),
            CGSize(width: 3000, height: 2000),
        ] {
            let origin = try #require(coordinates.location(
                .zero,
                in: bounds
            ))
            let center = try #require(coordinates.location(
                CGPoint(x: bounds.width / 2, y: bounds.height / 2),
                in: bounds
            ))
            let corner = try #require(coordinates.location(
                CGPoint(x: bounds.width, y: bounds.height),
                in: bounds
            ))
            #expect(origin.x == 0 && origin.y == 0)
            #expect(center.x == 16384 && center.y == 16384)
            #expect(corner.x == maximum && corner.y == maximum)
        }
    }

    @Test
    @MainActor
    func touchResolutionDoesNotCollapseAfterTheViewGrows() throws {
        // A launch-sized range of 440 columns would give a view grown to
        // 3000 points 440 distinct values; the fixed range keeps every point.
        let coordinates = GuestInputCoordinateSpace.touchscreen
        let bounds = CGSize(width: 3000, height: 2000)
        var previous: UInt32?
        for column in 0..<3000 {
            let location = try #require(coordinates.location(
                CGPoint(x: CGFloat(column) + 0.5, y: 1000),
                in: bounds
            ))
            if let previous { #expect(location.x > previous) }
            previous = location.x
        }
    }

    @Test
    @MainActor
    func touchPathReportsFullRangeCoordinates() throws {
        let geometry = try #require(GuestTouchGeometry(
            coordinateSpace: .touchscreen,
            boundsSize: CGSize(width: 500, height: 250),
            drawableSize: CGSize(width: 1000, height: 500),
            contentSize: CGSize(width: 800, height: 800)
        ))
        var path = GuestTouchPath()

        // The guest square spans bounds x 125...375 (drawable 250...750).
        #expect(path.begin(
            at: CGPoint(x: 250, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 16384, y: 16384, pressed: true),
        ])
        #expect(path.move(
            to: CGPoint(x: 200, y: 62.5),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 9830, y: 8192, pressed: true),
        ])
        // The path leaves through the right edge of the guest square, at the
        // top of the range's last column.
        #expect(path.end(
            at: CGPoint(x: 450, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 32767, y: 13926, pressed: false),
        ])
    }

    @Test
    @MainActor
    func touchPathStartsAndEndsAtGuestDisplayEdges() throws {
        let geometry = try #require(GuestTouchGeometry(
            coordinateSpace: GuestInputCoordinateSpace(
                size: CGSize(width: 400, height: 800)
            ),
            boundsSize: CGSize(width: 500, height: 250),
            drawableSize: CGSize(width: 1000, height: 500),
            contentSize: CGSize(width: 800, height: 800)
        ))
        var path = GuestTouchPath()

        #expect(path.begin(
            at: CGPoint(x: 50, y: 125),
            geometry: geometry
        ).isEmpty)
        #expect(path.move(
            to: CGPoint(x: 200, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 0, y: 400, pressed: true),
            GuestTouchEvent(x: 120, y: 400, pressed: true),
        ])
        #expect(path.move(
            to: CGPoint(x: 450, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 399, y: 400, pressed: false),
        ])
        #expect(path.move(
            to: CGPoint(x: 300, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 399, y: 400, pressed: true),
            GuestTouchEvent(x: 280, y: 400, pressed: true),
        ])
        #expect(path.cancel(
            at: CGPoint(x: 50, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 0, y: 400, pressed: false),
        ])
    }

    @Test
    @MainActor
    func coalescedTouchSegmentsCrossTheGuestDisplay() throws {
        let geometry = try #require(GuestTouchGeometry(
            coordinateSpace: GuestInputCoordinateSpace(
                size: CGSize(width: 400, height: 800)
            ),
            boundsSize: CGSize(width: 500, height: 250),
            drawableSize: CGSize(width: 1000, height: 500),
            contentSize: CGSize(width: 800, height: 800)
        ))
        var path = GuestTouchPath()

        #expect(path.begin(
            at: CGPoint(x: 50, y: 125),
            geometry: geometry
        ).isEmpty)
        #expect(path.move(
            to: CGPoint(x: 450, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 0, y: 400, pressed: true),
            GuestTouchEvent(x: 399, y: 400, pressed: false),
        ])
        #expect(path.end(
            at: CGPoint(x: 50, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 399, y: 400, pressed: true),
            GuestTouchEvent(x: 0, y: 400, pressed: false),
        ])
    }

    @Test
    @MainActor
    func coalescedCurvedTouchPathPreservesViewportCrossings() throws {
        let geometry = try #require(GuestTouchGeometry(
            coordinateSpace: GuestInputCoordinateSpace(
                size: CGSize(width: 400, height: 800)
            ),
            boundsSize: CGSize(width: 500, height: 250),
            drawableSize: CGSize(width: 1000, height: 500),
            contentSize: CGSize(width: 800, height: 800)
        ))
        let start = CGPoint(x: 50, y: 50)
        let firstInterior = CGPoint(x: 200, y: 75)
        let secondInterior = CGPoint(x: 300, y: 175)
        let current = CGPoint(x: 50, y: 200)

        var endpointOnlyPath = GuestTouchPath()
        #expect(endpointOnlyPath.begin(
            at: start,
            geometry: geometry
        ).isEmpty)
        #expect(endpointOnlyPath.move(
            to: current,
            geometry: geometry
        ).isEmpty)

        let samples = GuestTouchSamples.ordered(
            coalesced: [firstInterior, secondInterior, current],
            current: current
        )
        #expect(samples == [firstInterior, secondInterior, current])
        #expect(GuestTouchSamples.ordered(
            coalesced: [firstInterior, secondInterior],
            current: current
        ) == [firstInterior, secondInterior, current])

        var coalescedPath = GuestTouchPath()
        #expect(coalescedPath.begin(
            at: start,
            geometry: geometry
        ).isEmpty)
        #expect(coalescedPath.move(
            through: samples,
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 0, y: 200, pressed: true),
            GuestTouchEvent(x: 120, y: 240, pressed: true),
            GuestTouchEvent(x: 280, y: 560, pressed: true),
            GuestTouchEvent(x: 0, y: 616, pressed: false),
        ])
    }

    @Test
    @MainActor
    func endingTouchProcessesItsFinalViewportCrossing() throws {
        let geometry = try #require(GuestTouchGeometry(
            coordinateSpace: GuestInputCoordinateSpace(
                size: CGSize(width: 400, height: 800)
            ),
            boundsSize: CGSize(width: 500, height: 250),
            drawableSize: CGSize(width: 1000, height: 500),
            contentSize: CGSize(width: 800, height: 800)
        ))
        var path = GuestTouchPath()

        #expect(path.begin(
            at: CGPoint(x: 200, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 120, y: 400, pressed: true),
        ])
        #expect(path.end(
            at: CGPoint(x: 450, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 399, y: 400, pressed: false),
        ])

        #expect(path.begin(
            at: CGPoint(x: 50, y: 125),
            geometry: geometry
        ).isEmpty)
        #expect(path.end(
            at: CGPoint(x: 200, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 0, y: 400, pressed: true),
            GuestTouchEvent(x: 120, y: 400, pressed: true),
            GuestTouchEvent(x: 120, y: 400, pressed: false),
        ])
    }

    @Test
    @MainActor
    func theApplicationHasOneSceneBecauseItRunsOneGuest() throws {
        // The runtime boots one guest per process, and closing a window
        // stops it. With a second window, closing that one would end the
        // guest the first still shows, so iPadOS must not offer one.
        let manifest = try #require(
            Bundle.main.object(
                forInfoDictionaryKey: "UIApplicationSceneManifest"
            ) as? [String: Any]
        )
        #expect(manifest["UIApplicationSupportsMultipleScenes"] as? Bool == false)
    }

    @Test
    @MainActor
    func aTransitionHoldsTheGuestModeUntilItCompletes() {
        var hold = GuestResizeHold()

        let rotation = hold.transitionWillBegin()
        #expect(rotation.action == .begin)
        #expect(hold.transitionDidComplete(token: rotation.token) == .finish)
    }

    @Test
    @MainActor
    func aSupersededTransitionNeverFinishesTheHold() {
        var hold = GuestResizeHold()

        let first = hold.transitionWillBegin()
        let second = hold.transitionWillBegin()
        #expect(hold.transitionDidComplete(token: first.token) == .none)
        #expect(hold.transitionDidComplete(token: second.token) == .finish)
    }

    @Test
    @MainActor
    func anInteractiveWindowResizeHoldsTheModeThroughEveryTransition() {
        var hold = GuestResizeHold()

        #expect(hold.interactiveResizeChanged(true) == .begin)
        // Each size the drag passes through runs a transition. None of them
        // lets the guest follow that size.
        for _ in 0..<4 {
            let step = hold.transitionWillBegin()
            #expect(hold.transitionDidComplete(token: step.token) == .none)
        }
        #expect(hold.interactiveResizeChanged(false) == .finish)

        // The next transition is an ordinary one again.
        let rotation = hold.transitionWillBegin()
        #expect(hold.transitionDidComplete(token: rotation.token) == .finish)
    }

    @Test
    @MainActor
    func theEndOfAGestureWaitsForItsLastTransition() {
        var hold = GuestResizeHold()

        #expect(hold.interactiveResizeChanged(true) == .begin)
        let last = hold.transitionWillBegin()
        #expect(hold.interactiveResizeChanged(false) == .none)
        #expect(hold.transitionDidComplete(token: last.token) == .finish)
    }

    @Test
    @MainActor
    func repeatedGestureStatesRaiseOneBeginAndOneFinish() {
        var hold = GuestResizeHold()

        #expect(hold.interactiveResizeChanged(false) == .none)
        #expect(hold.interactiveResizeChanged(true) == .begin)
        #expect(hold.interactiveResizeChanged(true) == .none)
        #expect(hold.interactiveResizeChanged(false) == .finish)
        #expect(hold.interactiveResizeChanged(false) == .none)
    }

    @Test
    @MainActor
    func overlayDiskIsRecreatedAtRequestedSize() throws {
        let manager = FileManager.default
        let directory = manager.temporaryDirectory.appendingPathComponent(
            UUID().uuidString,
            isDirectory: true
        )
        try manager.createDirectory(
            at: directory,
            withIntermediateDirectories: true
        )
        defer { try? manager.removeItem(at: directory) }

        let first = try OverlayDisk.recreate(sizeGiB: 3, in: directory)
        let writer = try FileHandle(forWritingTo: first)
        try writer.write(contentsOf: Data([0xa5]))
        try writer.close()

        let second = try OverlayDisk.recreate(sizeGiB: 3, in: directory)
        let attributes = try manager.attributesOfItem(atPath: second.path)
        let size = attributes[.size] as? NSNumber
        #expect(first == second)
        #expect(size?.uint64Value == 3 * 1024 * 1024 * 1024)

        let reader = try FileHandle(forReadingFrom: second)
        let prefix = try reader.read(upToCount: 1)
        try reader.close()
        #expect(prefix == Data([0]))
    }

    @Test
    func diskUtilExpandsRawImageWithoutChangingItsPrefix() throws {
        let manager = FileManager.default
        let directory = manager.temporaryDirectory.appendingPathComponent(
            UUID().uuidString,
            isDirectory: true
        )
        try manager.createDirectory(
            at: directory,
            withIntermediateDirectories: true
        )
        defer { try? manager.removeItem(at: directory) }
        let image = directory.appendingPathComponent("disk.img")
        let original = Data([0x45, 0x46, 0x49, 0x20, 0x50, 0x41, 0x52, 0x54])
        try original.write(to: image)

        let diskUtil = DiskUtil()
        #expect(try diskUtil.resizeImage(at: image, toGiB: 1))
        #expect(try diskUtil.imageSize(at: image) == DiskUtil.bytesPerGiB)
        #expect(try !diskUtil.resizeImage(at: image, toGiB: 1))
        let reader = try FileHandle(forReadingFrom: image)
        let prefix = try reader.read(upToCount: original.count)
        try reader.close()
        #expect(prefix == original)
    }

    @Test
    func bundlePreparesItsDirectlyWritableDisk() throws {
        let manager = FileManager.default
        let directory = manager.temporaryDirectory.appendingPathComponent(
            UUID().uuidString,
            isDirectory: true
        )
        let guestDirectory = directory.appendingPathComponent("guest")
        try manager.createDirectory(
            at: guestDirectory,
            withIntermediateDirectories: true
        )
        defer { try? manager.removeItem(at: directory) }
        let disk = guestDirectory.appendingPathComponent("disk.img")
        #expect(manager.createFile(atPath: disk.path, contents: Data([1])))
        let bundle = GuestBundle(
            url: directory,
            flavor: .android,
            disk: disk
        )

        let diskUtil = StubDiskCapacityManager()
        #expect(
            try bundle.prepareDiskCapacity(
                1,
                diskUtil: diskUtil
            )
        )
    }

    @Test
    func androidBundleSelectionDoesNotRequireReleaseMetadata() throws {
        let manager = FileManager.default
        let directory = manager.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
            .appendingPathExtension(GuestBundle.pathExtension)
        let guestDirectory = directory.appendingPathComponent("guest")
        try manager.createDirectory(
            at: guestDirectory,
            withIntermediateDirectories: true
        )
        defer { try? manager.removeItem(at: directory) }
        let disk = guestDirectory.appendingPathComponent("legacy.img")
        #expect(manager.createFile(atPath: disk.path, contents: Data()))

        let bundle = GuestBundle.load(at: directory, fileManager: manager)

        #expect(bundle?.disk == disk)
        #expect(bundle?.flavor == .android)
    }

    @Test
    func configurationDefaultsUseMaximumCPUAndRequestedDefaults() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }

        let configuration = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6
        )

        #expect(configuration.vCPUCount == 6)
        #expect(
            configuration.guestMemoryMiB ==
                (ProcessInfo.processInfo.isiOSAppOnMac ? 4096 : 2048)
        )
        #expect(configuration.overlaySizeGiB == 8)
        #expect(configuration.androidDiskSizeGiB == 8)
        #expect(configuration.rootfsFilename == "kde-20260929-r2.erofs")
        #expect(configuration.retinaMode == false)
        #expect(configuration.autoResizeGuest == true)
        #expect(configuration.androidBootMode == .automatic)
    }

    @Test
    func configurationPersistsAndNormalizesStoredValues() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }

        EmulatorConfiguration(
            vCPUCount: 4,
            guestMemoryMiB: 2048,
            overlaySizeGiB: 16,
            androidDiskSizeGiB: 32,
            rootfsFilename: "phosh.erofs",
            retinaMode: true,
            autoResizeGuest: false,
            androidBootMode: .recovery
        ).save(defaults: defaults)
        #expect(
            EmulatorConfiguration.load(
                defaults: defaults,
                physicalProcessorCount: 6
            ) == EmulatorConfiguration(
                vCPUCount: 4,
                guestMemoryMiB: 2048,
                overlaySizeGiB: 16,
                androidDiskSizeGiB: 32,
                rootfsFilename: "phosh.erofs",
                    retinaMode: true,
                autoResizeGuest: false,
                androidBootMode: .recovery
            )
        )

        defaults.set(99, forKey: "emulator.vCPUCount")
        defaults.set(1000, forKey: "emulator.guestMemoryMiB")
        defaults.set(0, forKey: "emulator.overlaySizeGiB")
        defaults.set(128, forKey: "emulator.androidDiskSizeGiB")
        defaults.set("../phosh.erofs", forKey: "emulator.rootfsFilename")
        let normalized = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6
        )
        #expect(normalized.vCPUCount == 6)
        #expect(normalized.guestMemoryMiB == 1024)
        #expect(normalized.overlaySizeGiB == 1)
        #expect(normalized.androidDiskSizeGiB == 64)
        #expect(normalized.rootfsFilename == "kde-20260929-r2.erofs")
        #expect(normalized.retinaMode == true)
        #expect(normalized.autoResizeGuest == false)
        #expect(normalized.androidBootMode == .recovery)
    }

    @Test
    func storedUnifiedImageSelectionOutlivesTheDesktopDefault() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }
        defaults.set("rootfs.erofs", forKey: "emulator.rootfsFilename")

        let configuration = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6
        )

        #expect(configuration.rootfsFilename == "rootfs.erofs")
        configuration.save(defaults: defaults)
        #expect(
            defaults.string(forKey: "emulator.rootfsFilename") ==
                "rootfs.erofs"
        )
    }

    @Test
    func retiredKernelPageSizeSettingIsDroppedOnLoad() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }
        defaults.set(16384, forKey: "emulator.kernelPageSize")

        _ = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6
        )

        #expect(defaults.object(forKey: "emulator.kernelPageSize") == nil)
    }

    @Test
    func environmentCanFreezePerformanceConfiguration() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }
        defaults.set(2, forKey: "emulator.vCPUCount")
        defaults.set(2048, forKey: "emulator.guestMemoryMiB")
        defaults.set(16, forKey: "emulator.overlaySizeGiB")
        defaults.set(true, forKey: "emulator.retinaMode")
        defaults.set(false, forKey: "emulator.autoResizeGuest")
        defaults.set(
            AndroidBootMode.normal.rawValue,
            forKey: "emulator.androidBootMode"
        )

        let selected = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6,
            environment: [
                EmulatorConfiguration.vCPUEnvironmentKey: "4",
                EmulatorConfiguration.guestMemoryEnvironmentKey: "1536",
                EmulatorConfiguration.overlaySizeEnvironmentKey: "8",
                EmulatorConfiguration.retinaEnvironmentKey: "0",
                EmulatorConfiguration.autoResizeGuestEnvironmentKey: "1",
                AndroidBootMode.environmentKey: "recovery"
            ]
        )
        #expect(selected.vCPUCount == 4)
        #expect(selected.guestMemoryMiB == 1536)
        #expect(selected.overlaySizeGiB == 8)
        #expect(selected.retinaMode == false)
        #expect(selected.autoResizeGuest == true)
        #expect(selected.androidBootMode == .recovery)

        let invalid = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6,
            environment: [
                EmulatorConfiguration.vCPUEnvironmentKey: "7",
                EmulatorConfiguration.guestMemoryEnvironmentKey: "1500",
                EmulatorConfiguration.overlaySizeEnvironmentKey: "0",
                EmulatorConfiguration.retinaEnvironmentKey: "false",
                EmulatorConfiguration.autoResizeGuestEnvironmentKey: "true",
                AndroidBootMode.environmentKey: "invalid"
            ]
        )
        #expect(invalid.vCPUCount == 2)
        #expect(invalid.guestMemoryMiB == 2048)
        #expect(invalid.overlaySizeGiB == 16)
        #expect(invalid.retinaMode == true)
        #expect(invalid.autoResizeGuest == false)
        #expect(invalid.androidBootMode == .normal)
    }

    @Test
    func rootfsSelectionAcceptsOnlyDocumentsFilenames() {
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "phosh.erofs",
                environment: [:]
            ) == "phosh.erofs"
        )
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "pmos-sxmo.erofs",
                environment: [:]
            ) == "pmos-sxmo.erofs"
        )
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "rootfs-gl-gles.ext4",
                environment: [:]
            ) == "rootfs-gl-gles.ext4"
        )
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "rootfs.erofs",
                environment: [RootfsImage.environmentKey: "phosh.erofs"]
            ) == "phosh.erofs"
        )
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "rootfs.erofs",
                environment: [RootfsImage.environmentKey: "../phosh.erofs"]
            ) == nil
        )
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "rootfs.erofs",
                environment: [RootfsImage.environmentKey: "phosh.img"]
            ) == nil
        )
        // The name ends up in a UBD kernel argument, which these would split
        // or misread.
        for unusable in [
            "My Disk.ext4", "tab\tdisk.erofs", "a,b.erofs", "a:b.erofs",
            "quote\".erofs", "\u{00A0}nbsp.erofs", "\u{8800}.erofs"
        ] {
            #expect(RootfsImage.validatedFilename(unusable) == nil)
        }
        #expect(RootfsImage.validatedFilename("my-disk_2.ext4") == "my-disk_2.ext4")
    }

    @Test
    func rootfsEnumerationIncludesOnlyRegularSupportedImages() throws {
        let manager = FileManager.default
        let directory = manager.temporaryDirectory.appendingPathComponent(
            UUID().uuidString,
            isDirectory: true
        )
        try manager.createDirectory(
            at: directory,
            withIntermediateDirectories: true
        )
        defer { try? manager.removeItem(at: directory) }

        #expect(
            manager.createFile(
                atPath: directory.appendingPathComponent("phosh.erofs").path,
                contents: Data()
            )
        )
        #expect(
            manager.createFile(
                atPath: directory.appendingPathComponent("rootfs.erofs").path,
                contents: Data()
            )
        )
        #expect(
            manager.createFile(
                atPath: directory
                    .appendingPathComponent("pmos-sxmo.erofs").path,
                contents: Data()
            )
        )
        #expect(
            manager.createFile(
                atPath: directory
                    .appendingPathComponent("rootfs-gl-gles.ext4").path,
                contents: Data()
            )
        )
        #expect(
            manager.createFile(
                atPath: directory.appendingPathComponent("ignored.img").path,
                contents: Data()
            )
        )
        try manager.createDirectory(
            at: directory.appendingPathComponent("directory.erofs"),
            withIntermediateDirectories: false
        )

        #expect(
            RootfsImage.availableFilenames(in: directory) == [
                "phosh.erofs",
                "pmos-sxmo.erofs",
                "rootfs-gl-gles.ext4",
                "rootfs.erofs",
            ]
        )
    }

    @Test
    func zstdUtilReportsProgressAndCreatesSparseOutput() throws {
        let manager = FileManager.default
        let directory = manager.temporaryDirectory.appendingPathComponent(
            UUID().uuidString,
            isDirectory: true
        )
        try manager.createDirectory(
            at: directory,
            withIntermediateDirectories: true
        )
        defer { try? manager.removeItem(at: directory) }

        let source = directory.appendingPathComponent("disk.img.zst")
        let destination = directory.appendingPathComponent("disk.img")
        try Self.zstdFixture.write(to: source)
        var reports: [ZstdUtil.DecompressionProgress] = []

        try ZstdUtil.decompress(
            from: source,
            to: destination,
            sparse: true
        ) { reports.append($0) }

        let attributes = try manager.attributesOfItem(atPath: destination.path)
        #expect((attributes[.size] as? NSNumber)?.intValue == Self.zstdOutputSize)
        let handle = try FileHandle(forReadingFrom: destination)
        var status = stat()
        let statResult = Darwin.fstat(handle.fileDescriptor, &status)
        #expect(statResult == 0)
        #expect(
            (reports.last?.sparseBytesSkipped ?? 0) >
                UInt64(Self.zstdOutputSize - 256 * 1024)
        )
        #expect(Int(status.st_blocks) * 512 < Self.zstdOutputSize / 2)

        let prefix = try handle.read(upToCount: Self.zstdPrefix.count)
        try handle.seek(
            toOffset: UInt64(Self.zstdOutputSize - Self.zstdSuffix.count)
        )
        let suffix = try handle.read(upToCount: Self.zstdSuffix.count)
        try handle.close()
        #expect(prefix == Self.zstdPrefix)
        #expect(suffix == Self.zstdSuffix)
        #expect(reports.first?.compressedBytesConsumed == 0)
        #expect(
            reports.last?.compressedBytesConsumed ==
                UInt64(Self.zstdFixture.count)
        )
        #expect(
            reports.last?.compressedBytesTotal ==
                UInt64(Self.zstdFixture.count)
        )
        #expect(
            reports.last?.decompressedBytesWritten ==
                UInt64(Self.zstdOutputSize)
        )
        #expect(reports.last?.fractionCompleted == 1)
    }

    @Test
    func zstdUtilLeavesExistingDestinationUntouchedOnFailure() throws {
        let manager = FileManager.default
        let directory = manager.temporaryDirectory.appendingPathComponent(
            UUID().uuidString,
            isDirectory: true
        )
        try manager.createDirectory(
            at: directory,
            withIntermediateDirectories: true
        )
        defer { try? manager.removeItem(at: directory) }

        let source = directory.appendingPathComponent("invalid.zst")
        let destination = directory.appendingPathComponent("existing.img")
        let original = Data("existing destination".utf8)
        try Data([0x00, 0x01, 0x02, 0x03]).write(to: source)
        try original.write(to: destination)

        var failed = false
        do {
            try ZstdUtil.decompress(from: source, to: destination)
        } catch {
            failed = true
        }
        #expect(failed)
        #expect(try Data(contentsOf: destination) == original)
        #expect(
            try manager.contentsOfDirectory(atPath: directory.path).sorted() == [
                "existing.img",
                "invalid.zst",
            ]
        )
    }

    private func isolatedDefaults() -> (UserDefaults, String) {
        let suiteName = "KernariumTests.\(UUID().uuidString)"
        return (UserDefaults(suiteName: suiteName)!, suiteName)
    }
}
