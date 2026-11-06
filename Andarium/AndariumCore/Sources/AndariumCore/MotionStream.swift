// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// Private host-to-guest motion protocol on vsock 5556. Each 20-byte record is
/// little endian: "ANS1", Android sensor type (1 or 4), three IEEE float32 SI
/// values. The guest timestamps receipt in its own CLOCK_BOOTTIME domain.
public struct MotionSample: Sendable {
    public enum Sensor: UInt32, Sendable { case accelerometer = 1, gyroscope = 4 }
    /// The host interface orientation the virtual device is mounted to
    /// (`MotionMount` decides when it changes).
    public enum Axes: Equatable, Sendable { case portrait, portraitUpsideDown, landscapeLeft, landscapeRight }
    public let sensor: Sensor
    public let x: Float
    public let y: Float
    public let z: Float

    public init(sensor: Sensor, x: Double, y: Double, z: Double, axes: Axes = .portrait) {
        self.sensor = sensor
        // Core Motion reports gravity in g with the opposite acceleration
        // sign to Android. Both use right-handed, fixed device axes.
        let scale = sensor == .accelerometer ? -9.80665 : 1.0
        let (horizontal, vertical): (Double, Double) = switch axes {
        case .portrait: (x, y)
        case .portraitUpsideDown: (-x, -y)
        case .landscapeLeft: (y, -x)
        case .landscapeRight: (-y, x)
        }
        self.x = Float(horizontal * scale)
        self.y = Float(vertical * scale)
        self.z = Float(z * scale)
    }

    public var encoded: Data? {
        guard x.isFinite, y.isFinite, z.isFinite else { return nil }
        var data = Data([0x41, 0x4e, 0x53, 0x31])
        for word in [sensor.rawValue, x.bitPattern, y.bitPattern, z.bitPattern] {
            var little = word.littleEndian
            withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
        }
        return data
    }
}

/// Sends only the latest sample of each sensor, with bounded memory and no
/// blocking writes. A stop closes the stream and drops pending samples; a
/// connection completing after a stop cannot revive it. The opener must bound
/// its connect wait (the app uses 250 ms).
public final class MotionStream: @unchecked Sendable {
    public static let port: UInt32 = 5556
    private let opener: any GuestStreamOpener
    private let connectionChanged: @Sendable (Bool) -> Void
    private let lock = NSLock()
    private let queue = DispatchQueue(label: "andarium-motion-stream")
    private var timer: DispatchSourceTimer?
    private var generation: UInt64 = 0
    private var descriptor: Int32 = -1
    private var latest: [MotionSample.Sensor: Data] = [:]
    private var retryAt = ContinuousClock.now

    public init(
        opener: any GuestStreamOpener,
        connectionChanged: @escaping @Sendable (Bool) -> Void = { _ in }
    ) {
        self.opener = opener
        self.connectionChanged = connectionChanged
    }
    deinit { stop() }

    @discardableResult
    public func start() -> UInt64 {
        lock.withLock {
            guard timer == nil else { return generation }
            generation &+= 1
            let epoch = generation
            retryAt = .now
            let source = DispatchSource.makeTimerSource(queue: queue)
            source.schedule(deadline: .now(), repeating: .milliseconds(10))
            source.setEventHandler { [weak self] in self?.sendLatest(epoch: epoch) }
            timer = source
            source.resume()
            return epoch
        }
    }

    public func stop() {
        lock.withLock {
            generation &+= 1
            timer?.cancel()
            timer = nil
            latest.removeAll()
            closeStream()
        }
    }

    public func submit(_ sample: MotionSample, epoch: UInt64) {
        guard let data = sample.encoded else { return }
        lock.withLock {
            guard timer != nil, generation == epoch else { return }
            latest[sample.sensor] = data
        }
    }

    // Called only with the lock held, so a stop cannot close a reused fd.
    private func closeStream() {
        if descriptor >= 0 {
            Darwin.close(descriptor)
            descriptor = -1
        }
    }

    private func sendLatest(epoch: UInt64) {
        let connect = lock.withLock { () -> Bool in
            generation == epoch && timer != nil && descriptor < 0 && ContinuousClock.now >= retryAt
        }
        if connect {
            let opened = try? opener.openStream()
            let connected = lock.withLock { () -> Bool? in
                guard generation == epoch, timer != nil else {
                    if let opened { Darwin.close(opened) }
                    return nil
                }
                retryAt = .now + .seconds(1)
                if let opened {
                    var one: Int32 = 1
                    if setsockopt(opened, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout.size(ofValue: one))) == 0 {
                        descriptor = opened
                    } else {
                        Darwin.close(opened)
                    }
                }
                // Never replay samples collected while connecting.
                latest.removeAll()
                return descriptor >= 0
            }
            if let connected { connectionChanged(connected) }
        }
        let disconnected = lock.withLock { () -> Bool in
            guard generation == epoch, timer != nil, descriptor >= 0 else { return false }
            for sensor in [MotionSample.Sensor.accelerometer, .gyroscope] {
                guard let packet = latest.removeValue(forKey: sensor) else { continue }
                let sent = packet.withUnsafeBytes { Darwin.send(descriptor, $0.baseAddress, $0.count, MSG_DONTWAIT) }
                if sent != packet.count {
                    // Closing discards a partial record and avoids queuing old
                    // motion behind a stalled or paused guest.
                    closeStream()
                    latest.removeAll()
                    retryAt = .now + .seconds(1)
                    return true
                }
            }
            return false
        }
        if disconnected { connectionChanged(false) }
    }
}
