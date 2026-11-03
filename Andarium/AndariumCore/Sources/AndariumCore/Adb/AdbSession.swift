// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Ends a connection when the guest has shown no sign of life for too long, or
/// when the operation is cancelled. Both shut the transport down, which wakes
/// the operation's thread out of a blocking read. The limits are counted on the
/// guest's running time (`GuestRunClock`), so a guest the application paused
/// does not time out.
public final class AdbWatchdog: @unchecked Sendable {
    public enum Cause: Equatable, Sendable {
        case none, timedOut, cancelled
    }

    private let condition = NSCondition()
    private let clock: GuestRunClock
    private let tick: TimeInterval
    private var transport: (any AdbTransport)?
    private var idleLimit: Duration?
    private var overallDeadline: Duration?
    private var lastActivity: Duration
    private var armed = false
    private var cause = Cause.none
    private var stopped = false

    /// `tick` is the real time between looks at the limits.
    public init(clock: GuestRunClock, tick: TimeInterval = 0.25) {
        self.clock = clock
        self.tick = tick
        lastActivity = clock.now
        let thread = Thread { [weak self] in self?.watch() }
        thread.name = "andarium-adb-watchdog"
        thread.start()
    }

    public var currentCause: Cause {
        condition.withLock { cause }
    }

    /// The connection the limits end. A watchdog already triggered ends it at
    /// once.
    public func attach(_ transport: any AdbTransport) {
        let ended = condition.withLock { () -> Bool in
            self.transport = transport
            return cause != .none
        }
        if ended { transport.shutdown() }
    }

    /// Starts a phase: it ends when `idle` of running time passes without
    /// `touch`, or when `overall` of running time has passed since now.
    public func arm(idle: Duration?, overall: Duration? = nil) {
        condition.withLock {
            idleLimit = idle
            overallDeadline = overall.map { clock.now + $0 }
            lastActivity = clock.now
            armed = true
        }
    }

    public func disarm() {
        condition.withLock { armed = false }
    }

    /// A sign of life: restarts the idle limit.
    public func touch() {
        condition.withLock { lastActivity = clock.now }
    }

    /// Cancels the operation from any thread.
    public func cancel() {
        let target = condition.withLock { () -> (any AdbTransport)? in
            if cause == .none { cause = .cancelled }
            condition.broadcast()
            return transport
        }
        target?.shutdown()
    }

    /// Waits `seconds` of real time, less when the operation ends first.
    public func sleep(_ seconds: TimeInterval) {
        let end = Date(timeIntervalSinceNow: seconds)
        condition.withLock {
            while cause == .none, !stopped, Date() < end {
                _ = condition.wait(until: end)
            }
        }
    }

    public func stop() {
        condition.withLock {
            stopped = true
            condition.broadcast()
        }
    }

    private func watch() {
        condition.lock()
        defer { condition.unlock() }
        while !stopped {
            if armed, cause == .none {
                let now = clock.now
                let idle = idleLimit.map { now - lastActivity >= $0 } ?? false
                let late = overallDeadline.map { now >= $0 } ?? false
                if idle || late {
                    cause = .timedOut
                    condition.broadcast()
                    let target = transport
                    condition.unlock()
                    target?.shutdown()
                    condition.lock()
                }
            }
            _ = condition.wait(until: Date(timeIntervalSinceNow: tick))
        }
    }
}

/// One adb operation: it connects (retrying while adbd is not there yet),
/// does the handshake, runs the caller's work on the same thread, and reports
/// how it ended in `AdbError`s. Every operation has a session of its own and
/// leaves nothing behind: the connection is closed when it ends.
public final class AdbSession: @unchecked Sendable {
    /// Connect and handshake together may take this long, in running time.
    public static let defaultConnectLimit = Duration.seconds(20)

    private let connector: any AdbConnector
    private let signer: (any AdbSigner)?
    private let connectLimit: Duration
    private let retryInterval: TimeInterval
    public let clock: GuestRunClock
    public let watchdog: AdbWatchdog

    public init(
        connector: any AdbConnector, signer: (any AdbSigner)?, clock: GuestRunClock,
        connectLimit: Duration = AdbSession.defaultConnectLimit, retryInterval: TimeInterval = 1,
        tick: TimeInterval = 0.25
    ) {
        self.connector = connector
        self.signer = signer
        self.clock = clock
        self.connectLimit = connectLimit
        self.retryInterval = retryInterval
        watchdog = AdbWatchdog(clock: clock, tick: tick)
    }

    /// Cancels the operation from any thread. The call that is running throws
    /// `AdbError.cancelled`.
    public func cancel() {
        watchdog.cancel()
    }

    /// Connects, runs `body` and closes. `body` arms the watchdog for each of
    /// its phases (`watchdog.arm`) and touches it on progress; until it does,
    /// nothing limits it.
    public func run<Result>(_ body: (AdbConnection) throws -> Result) throws -> Result {
        defer { watchdog.stop() }
        var transport: (any AdbTransport)?
        defer { transport?.shutdown() }
        do {
            watchdog.arm(idle: nil, overall: connectLimit)
            let opened = try openTransport()
            transport = opened
            watchdog.attach(opened)
            let connection = AdbConnection(transport: opened)
            try connection.handshake(signer: signer)
            watchdog.disarm()
            return try body(connection)
        } catch {
            throw translated(error)
        }
    }

    private func openTransport() throws -> any AdbTransport {
        while true {
            if let ended = ended() { throw ended }
            do {
                return try connector.connect()
            } catch AdbError.refused {
                // adbd may still be starting, or the guest waking: ask again.
                if let ended = ended() { throw ended }
                watchdog.sleep(retryInterval)
            }
        }
    }

    private func ended() -> AdbError? {
        switch watchdog.currentCause {
        case .none: nil
        case .timedOut: .timedOut
        case .cancelled: .cancelled
        }
    }

    /// An error from a connection that the watchdog ended says why it ended.
    private func translated(_ error: Error) -> Error {
        if let ended = ended() { return ended }
        return error
    }
}
