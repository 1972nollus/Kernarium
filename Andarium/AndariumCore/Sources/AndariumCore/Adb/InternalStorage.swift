// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Whether Android has brought its internal storage up. Android reports the
/// end of its boot before it: for some seconds after that, `/storage/emulated/0`
/// is an empty, read-only folder, and then a mount (the media provider's FUSE
/// daemon) fills it in, creates the standard folders, and lets the shell write.
/// A listing or an import made before is false: an empty root, a refused write.
public enum InternalStorage {
    /// What to run in the guest's shell: it prints `ready` once the storage is
    /// the FUSE mount, the shell can write to it and the standard folders are
    /// made, and `starting` before.
    public static let command = """
        if grep -q ' /storage/emulated fuse' /proc/mounts && [ -w /storage/emulated/0 ] \
        && [ -d /storage/emulated/0/Download ] && [ -d /storage/emulated/0/DCIM ] \
        && [ -d /storage/emulated/0/Pictures ]; then echo ready; else echo starting; fi
        """

    /// Whether the storage is ready, from the command's output: nil for
    /// anything else.
    public static func parse(_ output: String) -> Bool? {
        switch output.trimmingCharacters(in: .whitespacesAndNewlines) {
        case "ready": true
        case "starting": false
        default: nil
        }
    }

    /// The same from a finished command: nil when it failed.
    public static func parse(_ result: ShellResult) -> Bool? {
        guard result.exitCode == 0 else { return nil }
        return parse(result.text)
    }
}
