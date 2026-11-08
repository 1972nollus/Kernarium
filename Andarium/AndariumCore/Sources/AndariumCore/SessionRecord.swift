// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// How the previous process's guest session ended, as the runtime recorded
/// it (darwin/runtime/session_record.hpp).
///
/// The runtime writes "running" when the guest boots and overwrites it with
/// the end on every way the session ends that runs any code: Android's own
/// power-off or restart, a stop, a guest kernel panic with its message, an
/// exit the kernel drives, a hibernated guest whose RAM could not be
/// restored, and an exit() of the process. A record still running at the
/// next launch says that the system or the person ended the process without
/// letting it run, as an end at the memory limit does.
public struct SessionRecord: Equatable, Sendable {
    public var running: Bool
    /// The runtime's word for the end: running, powered-off, restarted,
    /// stopped, failed, guest-panic, kernel-abort, kernel-exit,
    /// hibernation-restore-failed, process-exit or host-fault.
    public var end: String
    public var status: Int
    public var processID: Int
    public var started: Date?
    public var ended: Date?
    /// The panic's message, or what else the end said.
    public var message: String?
    /// The guest kernel's last output before a crash, oldest line first.
    public var kernelOutput: String?

    /// Reads a record's text; nil when it is not one.
    public init?(text: String) {
        let parts = text.components(separatedBy: "\nkernel-output\n")
        var fields: [String: String] = [:]
        let lines = parts[0].split(separator: "\n", omittingEmptySubsequences: true)
        guard lines.first == "uml-session 1" else { return nil }
        for line in lines.dropFirst() {
            guard let space = line.firstIndex(of: " ") else { continue }
            fields[String(line[..<space])] = String(line[line.index(after: space)...])
        }
        guard let state = fields["state"], let end = fields["end"] else { return nil }
        running = state == "running"
        self.end = end
        status = fields["status"].flatMap { Int($0) } ?? 0
        processID = fields["pid"].flatMap { Int($0) } ?? 0
        started = fields["started"].flatMap { TimeInterval($0) }.map { Date(timeIntervalSince1970: $0) }
        ended = fields["ended"].flatMap { TimeInterval($0) }.map { Date(timeIntervalSince1970: $0) }
        message = fields["message"]
        kernelOutput = parts.count > 1 ? parts[1] : nil
    }

    /// The record at `url`, or nil when there is none or it is not a record.
    public static func read(at url: URL) -> SessionRecord? {
        guard let data = try? Data(contentsOf: url), let text = String(data: data, encoding: .utf8) else {
            return nil
        }
        return SessionRecord(text: text)
    }

    /// Whether the session ended on its own terms: Android's power-off or
    /// restart, or a stop.
    public var orderly: Bool {
        !running && ["powered-off", "restarted", "stopped"].contains(end)
    }

    /// One line for a log or a report.
    public var summary: String {
        if running {
            return "The previous session was still running when the process ended (pid \(processID)): "
                + "the system or the person ended it, with no crash report from Andarium."
        }
        let detail = message.map { ": \($0)" } ?? ""
        switch end {
        case "guest-panic":
            return "The previous session ended in a guest kernel panic\(detail)."
        case "kernel-abort":
            return "The previous session ended in a guest kernel crash\(detail)."
        case "kernel-exit":
            return "The guest kernel ended the previous process with status \(status)\(detail)."
        case "hibernation-restore-failed":
            return "The previous session ended because its guest RAM could not be restored\(detail)."
        case "process-exit":
            return "The previous process exited while its guest ran\(detail)."
        case "host-fault":
            return "The previous process crashed in Andarium's own code, with a crash report\(detail)."
        default:
            return "The previous session ended: \(end)\(detail)."
        }
    }
}
