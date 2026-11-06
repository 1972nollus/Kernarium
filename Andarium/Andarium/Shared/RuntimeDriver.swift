import AndariumCore
import Foundation

/// What the Cancel button of a long task does. Two are equal when they are the
/// same button, so a state that only changes its words stays equal to itself.
struct BusyCancel: Equatable {
    let id = UUID()
    let action: @MainActor () -> Void

    static func == (lhs: BusyCancel, rhs: BusyCancel) -> Bool { lhs.id == rhs.id }
}

/// A long task the screens wait for: creating or erasing a guest, or one of
/// the guest's own operations.
struct BusyState: Equatable {
    var title: String
    var detail: String
    /// 0 to 1, or nil while the length of the task is not known.
    var progress: Double?
    /// Shows a Cancel button that does this; nil for a task that cannot stop.
    var cancel: BusyCancel?
}

/// What a running operation tells the overlay: any part may be left out.
nonisolated struct BusyUpdate: Sendable {
    var progress: Double?
    var detail: String?
    /// False once the task is past the point where it can stop; true again is
    /// never sent.
    var cancellable: Bool?

    init(progress: Double? = nil, detail: String? = nil, cancellable: Bool? = nil) {
        self.progress = progress
        self.detail = detail
        self.cancellable = cancellable
    }
}

/// Something the person should know and acknowledge.
struct AppAlert: Identifiable, Equatable {
    let id = UUID()
    var title: String
    var message: String
}

/// How far the system lets Andarium use the microphone.
enum MicrophoneState: Equatable {
    case notAsked, denied, granted
}

/// A question about a permission iOS has already refused, which only the
/// person can change in Settings.
enum PermissionPrompt: Identifiable, Equatable {
    case microphone

    var id: Self { self }
}

/// What the device application does behind the screens: the guest's files and
/// its run. The Simulator preview has no driver, and the model then plays the
/// same steps against mock data.
@MainActor
protocol RuntimeDriver: AnyObject {
    func create(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws -> Guest
    func erase(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws -> Guest
    func save(_ guest: Guest) throws
    /// What a start would be, from the guest's files; nil when they cannot be
    /// read, which a start then reports.
    func startPlan() -> StartPlan?
    /// A run is wanted. The runtime starts once the guest's display has its
    /// size, after the disk is ready.
    func begin(guest: Guest, mode: StartMode, kind: BootKind)
    /// Puts the data back to what the update that did not finish started
    /// from; the guest does not start.
    func restoreUpdate() throws
    /// Enables or stops sharing motion with the guest.
    func setMotionSensors(_ on: Bool)
    /// The scene's geometry changed, which may include its interface
    /// orientation.
    func sceneGeometryDidChange()
    /// Turns the guest's microphone on or off, asking the system for access
    /// when the person turns it on for the first time.
    func setMicrophone(_ on: Bool)
    /// Asks the guest for a bug report and copies it into `BugReportStaging`.
    /// Returns the staged file; cancelling the task stops the report.
    func createBugReport(update: @escaping @Sendable (BusyUpdate) -> Void) async throws -> URL
    /// Installs the APK at `url`, which the caller can read, into the guest.
    /// Cancelling the task stops it while the file is still being sent.
    func installApk(at url: URL, sizeBytes: Int, update: @escaping @Sendable (BusyUpdate) -> Void) async throws
    /// Whether the guest's USB debugging setting is on; nil when it cannot be
    /// read now.
    func usbDebugging() async -> Bool?
    /// Opens the listener of ADB over the network on the device's network
    /// addresses. Throws when it cannot (`AdbError.io(EADDRINUSE)` for a
    /// port in use).
    func startAdbOverNetwork() throws
    /// Closes the listener and every computer's connection through it.
    func stopAdbOverNetwork()
    /// The device's IPv4 address on a local network, which a computer
    /// connects to; nil without one.
    func localNetworkAddress() -> String?
    /// One reading of the running guest's host-side figures, taken off the
    /// main thread; nil when no guest runs.
    func readStatistics() async -> StatsSample?
    /// The guest kernel's own memory figures; nil when they could not be
    /// had: the kernel does not answer the request yet, or asking could wake
    /// a guest that sleeps. The caller keeps the last figures then. It may
    /// wait up to half a second, and never on the main thread.
    func readGuestMemory() async -> GuestMemorySample?
}

/// What a screen needs from its launch arguments to open a state directly.
struct ScreenHints {
    var scrollTarget: String?
    var eraseConfirmed = false
    var infoPage: InfoPage?
}
