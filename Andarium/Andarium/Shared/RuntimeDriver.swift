import AndariumCore
import Foundation

/// A long task the screens wait for: creating or erasing a guest.
struct BusyState: Equatable {
    var title: String
    var detail: String
    /// 0 to 1, or nil while the length of the task is not known.
    var progress: Double?
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
    /// Turns the guest's microphone on or off, asking the system for access
    /// when the person turns it on for the first time.
    func setMicrophone(_ on: Bool)
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
    var acknowledgements = false
}
