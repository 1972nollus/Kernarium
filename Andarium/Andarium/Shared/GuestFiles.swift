// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import SwiftUI

/// Paths inside the guest's internal storage, which is all the browser shows.
nonisolated enum GuestPath {
    /// What the guest's Files app shows as internal storage. The browser
    /// cannot go above it.
    static let root = "/storage/emulated/0"

    static func child(_ name: String, of folder: String) -> String { folder + "/" + name }

    static func name(of path: String) -> String {
        path == root ? "Internal storage" : String(path.split(separator: "/").last ?? "")
    }

    static func parent(of path: String) -> String {
        path == root ? root : String(path[..<(path.lastIndex(of: "/") ?? path.endIndex)])
    }

    /// The name of a file's folder, for a list that mixes folders.
    static func parentName(of path: String) -> String { name(of: parent(of: path)) }
}

/// One entry of a folder, as the sync service lists it.
nonisolated struct GuestFile: Identifiable, Hashable, Sendable {
    var path: String
    var isFolder: Bool
    var size: UInt64
    var modified: Date

    var id: String { path }
    var name: String { GuestPath.name(of: path) }
}

/// Why a folder has no listing.
nonisolated enum GuestFolderProblem: Error, Equatable {
    /// Android refuses the shell user this folder: `Android/data` and
    /// `Android/obb`.
    case refused
    case unavailable(String)
}

/// A file the document picker gave back. The preview's have no URL.
nonisolated struct FilePick: Identifiable, Sendable {
    var url: URL?
    var name: String
    var size: UInt64

    var id: String { name }
}

/// What the browser asks of the guest. The device's goes over the app's adb
/// channel (`LIS2`, `RECV`, `SEND` and `df` in a shell); the Simulator
/// preview's is a tree of mock files.
protocol GuestFilesBackend: AnyObject {
    func list(_ folder: String) async throws -> [GuestFile]

    /// Free space of internal storage; nil when it cannot be read now.
    func freeBytes() async -> UInt64?

    /// The bytes an export of `file` moves: a folder is walked.
    func measure(_ file: GuestFile) async throws -> UInt64

    /// Writes `pick` at `path`, replacing what is there. On cancellation or
    /// failure nothing is left at `path`.
    func send(_ pick: FilePick, to path: String, progress: @escaping @Sendable (UInt64) -> Void) async throws

    /// Copies `file` into the local `directory`, a folder with its whole
    /// tree, and returns what it made.
    func receive(_ file: GuestFile, into directory: URL, progress: @escaping @Sendable (UInt64) -> Void) async throws -> URL

    /// What Import opens instead of the system's document picker: the
    /// Simulator has no documents to pick. Nil on a device.
    func importPickerStandIn(deliver: @escaping ([FilePick]) -> Void) -> AnyView?
}
