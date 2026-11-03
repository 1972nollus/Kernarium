import AndariumCore
import Foundation

/// Opens connections to the guest's adbd through the runtime: a connected
/// stream socket to the guest's vsock port, which only this process holds.
nonisolated struct VsockConnector: AdbConnector {
    static let adbPort: UInt32 = 5555
    /// How long one attempt waits for the guest to answer; the session asks
    /// again until its connect limit.
    static let attemptTimeoutMilliseconds: UInt32 = 3000

    func connect() throws -> any AdbTransport {
        DescriptorTransport(descriptor: try Self.open(timeout: Self.attemptTimeoutMilliseconds))
    }

    private static func open(timeout: UInt32) throws -> Int32 {
        let result = kernarium_runtime_loader_connect_vsock(adbPort, timeout)
        if result >= 0 { return result }
        switch -result {
        case ECONNREFUSED, ETIMEDOUT: throw AdbError.refused
        case ENODEV, ECONNRESET: throw AdbError.guestGone
        default: throw AdbError.io(-result)
        }
    }
}

/// Runs an adb operation on a thread of its own, behind `async`: the I/O
/// blocks, and a task cancelling ends the connection, which wakes the thread.
nonisolated enum AdbOperationRunner {
    static func run<Output: Sendable>(
        identity: AdbKeyStore.Identity?, clock: GuestRunClock, cancellable: Bool = true,
        connector: any AdbConnector = VsockConnector(), onCancel: @escaping @Sendable () -> Void = {},
        _ body: @escaping @Sendable (AdbSession) throws -> Output
    ) async throws -> Output {
        let session = AdbSession(connector: connector, signer: identity?.signer, clock: clock)
        let operation: () async throws -> Output = {
            try await withCheckedThrowingContinuation { continuation in
                let thread = Thread {
                    defer { session.watchdog.stop() }
                    do {
                        continuation.resume(returning: try body(session))
                    } catch {
                        continuation.resume(throwing: error)
                    }
                }
                thread.name = "andarium-adb"
                thread.qualityOfService = .userInitiated
                thread.start()
            }
        }
        guard cancellable else { return try await operation() }
        return try await withTaskCancellationHandler(operation: operation) {
            session.cancel()
            onCancel()
        }
    }

    /// An operation's error in the form the model shows: a cancel is a
    /// `CancellationError`, an adb failure has its words, and anything else
    /// (no room for the report) says what it says.
    static func presented(_ error: Error, activity: AdbActivity) -> Error {
        if error is CancellationError { return error }
        if let cocoa = error as? CocoaError, cocoa.code == .userCancelled { return CancellationError() }
        guard let adb = error as? AdbError else { return error }
        return adb == .cancelled ? CancellationError() : AdbFailure(adb, activity: activity)
    }
}

/// The operations the screens ask of Android over adb, with the key this boot
/// carries and the clock of the guest's running time. The application builds
/// one for each call; the hosted tests build their own and drive a real guest
/// through the same code.
nonisolated struct AdbGuestOperations: Sendable {
    var identity: AdbKeyStore.Identity?
    var clock: GuestRunClock
    /// "iPad", "iPhone" or "Mac", for the words of a failure.
    var device: String
    var connector: any AdbConnector = VsockConnector()

    /// Runs a command in the guest's shell and returns what it printed.
    func shell(_ command: String, limit: Duration = .seconds(60)) async throws -> ShellResult {
        guard let identity else { throw AdbFailure(.keyRejected, activity: .bugReport) }
        return try await AdbOperationRunner.run(identity: identity, clock: clock, connector: connector) { session in
            try session.run { connection in
                session.watchdog.arm(idle: limit, overall: limit)
                return try connection.runShell(command)
            }
        }
    }

}
