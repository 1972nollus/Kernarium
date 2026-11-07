// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// Opt-in real-guest test of the host's motion session against the guest's
/// own sensor service, with a synthetic device (the Mac has no sensors): run
/// with `ANDARIUM_MOTION_COMPAT=1`, `ANDARIUM_START=normal` and an isolated
/// instance. The guest's service is reached through the runtime's vsock, as
/// in the application.
///
/// `ANDARIUM_MOTION_EXPECT=legacy` (the default) is an image whose service
/// never sends a control frame: the session reads both sensors at 100 Hz for
/// as long as the connection holds. `ondemand` is an image whose service
/// sends them: a frame arrives on connect, and the sensors the guest does not
/// use are not read.
@Suite(.serialized)
@MainActor
struct MotionCompatibilityTests {
    private struct Connector: GuestStreamOpener {
        func openStream() throws -> Int32 {
            let descriptor = kernarium_runtime_loader_connect_vsock(MotionStream.port, 250)
            guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: -descriptor) ?? .EIO) }
            return descriptor
        }
    }

    private final class SyntheticDevice: MotionDevice, @unchecked Sendable {
        let hasAccelerometer = true
        let hasGyroscope = true
        private let lock = NSLock()
        private var running: [MotionSample.Sensor: TimeInterval] = [:]
        private var handlers: [MotionSample.Sensor: @Sendable (Double, Double, Double) -> Void] = [:]
        private var timer: DispatchSourceTimer?
        private(set) var emitted = 0

        func start(
            _ sensor: MotionSample.Sensor, interval: TimeInterval,
            handler: @escaping @Sendable (Double, Double, Double) -> Void, failure: @escaping @Sendable () -> Void
        ) {
            lock.withLock {
                running[sensor] = interval
                handlers[sensor] = handler
            }
        }

        func stop(_ sensor: MotionSample.Sensor) {
            lock.withLock {
                running[sensor] = nil
                handlers[sensor] = nil
            }
        }

        var intervals: [MotionSample.Sensor: TimeInterval] { lock.withLock { running } }

        /// Delivers every running sensor at its own interval, as Core Motion would.
        func run() {
            let source = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "synthetic-motion"))
            source.schedule(deadline: .now(), repeating: .milliseconds(10))
            source.setEventHandler { [self] in
                let current = lock.withLock { handlers }
                for (sensor, handler) in current {
                    handler(0, 0, sensor == .accelerometer ? -1 : 0.1)
                    lock.withLock { emitted += 1 }
                }
            }
            source.resume()
            timer = source
        }

        func finish() { timer?.cancel() }
    }

    @Test(.enabled(if: ProcessInfo.processInfo.environment["ANDARIUM_MOTION_COMPAT"] == "1"), .timeLimit(.minutes(20)))
    func theSessionAndTheGuestsSensorServiceAgree() async throws {
        let expectation = ProcessInfo.processInfo.environment["ANDARIUM_MOTION_EXPECT"] ?? "legacy"
        try await withAndroidGuest { _, _ in
            let device = SyntheticDevice()
            device.run()
            defer { device.finish() }
            let session = MotionSession(opener: Connector(), device: device) { problem in
                print("MOTION compat: problem \(String(describing: problem))")
            }
            session.update(enabled: true, foreground: true, axes: .portrait)
            defer { session.update(enabled: false, foreground: false, axes: .portrait) }
            // Connected: the sensors start, continuously until the service speaks.
            let connectDeadline = ContinuousClock.now + .seconds(30)
            while session.plan == .off, ContinuousClock.now < connectDeadline {
                try await Task.sleep(for: .milliseconds(50))
            }
            try #require(session.plan != .off, "The guest's motion service did not accept the connection")
            print("MOTION compat: connected; plan \(session.plan)")
            // Hold the connection and watch what the service says for a while.
            let watchUntil = ContinuousClock.now + .seconds(8)
            var seen: [String] = []
            while ContinuousClock.now < watchUntil {
                let line = "\(String(describing: session.demand)) \(session.plan)"
                if seen.last != line { seen.append(line) }
                try await Task.sleep(for: .milliseconds(100))
            }
            print("MOTION compat: observed \(seen)")
            print("MOTION compat: \(device.emitted) readings delivered")
            switch expectation {
            case "legacy":
                #expect(session.demand == nil)
                #expect(session.plan == MotionPlan(accelerometer: 0.01, gyroscope: 0.01))
                #expect(seen.count == 1)
                #expect(device.emitted > 100)
            default:
                try #require(session.demand != nil, "No control frame arrived on the connection")
                let demand = try #require(session.demand)
                #expect((demand.accelerometer == nil) == (session.plan.accelerometer == nil))
                #expect((demand.gyroscope == nil) == (session.plan.gyroscope == nil))
            }
        }
    }
}
