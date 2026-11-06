// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CoreMotion
import Foundation

private nonisolated struct MotionConnector: GuestStreamOpener {
    func openStream() throws -> Int32 {
        let descriptor = kernarium_runtime_loader_connect_vsock(MotionStream.port, 250)
        guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: -descriptor) ?? .EIO) }
        return descriptor
    }
}

/// Core Motion is opened only for an opted-in, booted, foreground guest.
/// The axes are set before a start and never change per sample (`MotionMount`
/// decides when they move). No samples are retained across backgrounding,
/// power-off, or a guest restart.
@MainActor
final class MotionSensors {
    private let manager = CMMotionManager()
    private let queue: OperationQueue = {
        let queue = OperationQueue()
        queue.name = "andarium-motion"
        queue.maxConcurrentOperationCount = 1
        return queue
    }()
    private lazy var stream = MotionStream(opener: MotionConnector()) { [weak self] connected in
        Task { @MainActor [weak self] in
            guard let self, self.running else { return }
            self.problem?(connected ? nil : "The guest’s motion sensor service is unavailable. Use a system image with motion sensor support.")
        }
    }
    private var running = false
    private var epoch: UInt64 = 0
    var problem: ((String?) -> Void)?
    var axes = MotionSample.Axes.portrait

    var available: Bool { manager.isAccelerometerAvailable || manager.isGyroAvailable }

    func start() {
        guard !running, available else { return }
        running = true
        epoch &+= 1
        let current = epoch
        problem?(nil)
        let streamEpoch = stream.start()
        let stream = stream
        let axes = axes
        let failed: @Sendable (Error?) -> Void = { [weak self] error in
            guard error != nil else { return }
            Task { @MainActor [weak self] in
                guard let self, self.running, self.epoch == current else { return }
                self.stop()
                self.problem?("The device’s motion sensors could not be read. Turn Gyroscope off and on to try again.")
            }
        }
        if manager.isAccelerometerAvailable {
            manager.accelerometerUpdateInterval = 0.01
            manager.startAccelerometerUpdates(to: queue) { data, error in
                failed(error)
                guard let data else { return }
                let a = data.acceleration
                stream.submit(MotionSample(sensor: .accelerometer, x: a.x, y: a.y, z: a.z, axes: axes), epoch: streamEpoch)
            }
        }
        if manager.isGyroAvailable {
            manager.gyroUpdateInterval = 0.01
            manager.startGyroUpdates(to: queue) { data, error in
                failed(error)
                guard let data else { return }
                let g = data.rotationRate
                stream.submit(MotionSample(sensor: .gyroscope, x: g.x, y: g.y, z: g.z, axes: axes), epoch: streamEpoch)
            }
        }
    }

    func stop() {
        running = false
        epoch &+= 1
        manager.stopAccelerometerUpdates()
        manager.stopGyroUpdates()
        queue.cancelAllOperations()
        stream.stop()
    }
}
