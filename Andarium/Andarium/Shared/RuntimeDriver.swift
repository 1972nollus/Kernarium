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
    /// A run is wanted. The runtime starts once the guest's display has its
    /// size, after the disk is ready.
    func begin(guest: Guest, mode: StartMode, kind: BootKind)
    /// Turns the guest's microphone on or off, asking the system for access
    /// when the person turns it on for the first time.
    func setMicrophone(_ on: Bool)
}

/// What a screen needs from its launch arguments to open a state directly.
struct ScreenHints {
    var scrollTarget: String?
    var eraseConfirmed = false
    var acknowledgements = false
}
