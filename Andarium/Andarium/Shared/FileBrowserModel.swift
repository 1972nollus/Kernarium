// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Observation
import UIKit

/// One folder on the Settings sheet's navigation stack.
struct FolderRoute: Hashable {
    var path: String
}

enum FolderListing: Equatable {
    case loading
    case loaded([GuestFile])
    case refused
    case failed(String)
}

enum FileSort: String, CaseIterable, Identifiable {
    case name, date, size

    var id: String { rawValue }

    var title: String {
        switch self {
        case .name: "Name"
        case .date: "Date Modified"
        case .size: "Size"
        }
    }
}

/// An import that waits for an answer: some of its names are in the folder.
struct ImportConflict {
    var picks: [FilePick]
    var clashing: [FilePick]
    var folder: String
}

enum ConflictChoice {
    case skip, keepBoth, replace
}

/// One import or export. There is one at a time; it runs while the person
/// goes on browsing.
@Observable
final class FileTransfer: Identifiable {
    enum Direction {
        case importing, exporting
    }

    enum Stage: Equatable {
        case running
        /// An export is staged and waits for the system's action menu.
        case ready
        case finished
        case failed
    }

    enum ItemState: Equatable {
        case waiting, moving, done
        case failed(String)
    }

    struct Item: Identifiable, Equatable {
        let id = UUID()
        var name: String
        var size: UInt64
        var isFolder = false
        var moved: UInt64 = 0
        var state: ItemState = .waiting
    }

    let id = UUID()
    let direction: Direction
    /// Where an import goes; an export names the folder it was started from.
    let folder: String
    var items: [Item]
    var stage = Stage.running
    /// The folders of an export are still being walked for their sizes.
    var measuring = false
    /// The card shows every item.
    var expanded = false
    /// iOS stopped the guest while Andarium was away, and the transfer with it.
    var pausedWhileAway = false
    /// The staged copies of an export, for the action menu.
    var staged: [URL] = []
    /// A held screen stays as it is.
    var stays = false
    @ObservationIgnored var task: Task<Void, Never>?

    init(direction: Direction, folder: String, items: [Item]) {
        self.direction = direction
        self.folder = folder
        self.items = items
    }

    var running: Bool { stage == .running }
    var totalBytes: UInt64 { items.reduce(0) { $0 + $1.size } }
    var movedBytes: UInt64 { items.reduce(0) { $0 + $1.moved } }

    var fraction: Double {
        guard totalBytes > 0 else { return 0 }
        return min(1, Double(movedBytes) / Double(totalBytes))
    }

    private var failures: [Item] {
        items.filter { if case .failed = $0.state { true } else { false } }
    }

    /// "Clip.mov", "3 files" or "2 items" when a folder is among them.
    private func what(_ items: [Item]) -> String {
        if items.count == 1, let only = items.first { return only.name }
        return "\(items.count) \(items.contains(where: \.isFolder) ? "items" : "files")"
    }

    private var verb: String { direction == .importing ? "Importing" : "Exporting" }
    private var done: String { direction == .importing ? "imported" : "exported" }

    /// Short where buttons share the line: the name of a single file moves to
    /// the detail line.
    var title: String {
        let single = items.count == 1
        switch stage {
        case .running: return "\(verb) \(what(items))"
        case .ready: return single ? "Ready to share" : "\(what(items)) ready"
        case .finished: return "\(what(items)) \(done)"
        case .failed: return single ? "Not \(done)" : "\(failures.count) of \(items.count) not \(done)"
        }
    }

    var detail: String {
        switch stage {
        case .running:
            if pausedWhileAway { return "Paused while Andarium was in the background" }
            if measuring { return "Counting files\u{2026}" }
            let moved = "\(Formats.size(bytes: movedBytes)) of \(Formats.size(bytes: totalBytes))"
            guard items.count > 1, let index = items.firstIndex(where: { $0.state == .moving }) else { return moved }
            return "\(index + 1) of \(items.count) \u{00B7} \(moved)"
        case .ready:
            let size = Formats.size(bytes: totalBytes)
            if items.count == 1, let only = items.first { return "\(only.name) \u{00B7} \(size)" }
            return size
        case .finished:
            return direction == .importing ? "In \(GuestPath.name(of: folder))" : Formats.size(bytes: totalBytes)
        case .failed:
            if case let .failed(reason)? = failures.first?.state { return reason }
            return ""
        }
    }
}

/// The file browser behind Settings: where the person is, what is selected
/// and the one transfer. It outlives a closed sheet, so a run's folders do not
/// load again.
@Observable
final class FileBrowserModel {
    @ObservationIgnored let backend: any GuestFilesBackend

    /// The Settings sheet's navigation path: empty on Settings itself, then
    /// internal storage and the folders below it.
    var route: [FolderRoute] = []
    var sort = FileSort.name
    /// What Export takes: the selection follows the person between folders.
    var selection: [GuestFile] = []
    var reviewingSelection = false
    var picking = false
    var conflict: ImportConflict?
    var transfer: FileTransfer?
    /// What the last import brought, marked in its folder.
    var fresh: Set<String> = []
    private(set) var freeBytes: UInt64?
    private var listings: [String: FolderListing] = [:]

    /// The system's action menu; a test or a preview may put its own here.
    @ObservationIgnored
    var presentShare: ([URL], @escaping @MainActor (Bool) -> Void) -> Bool = { files, finished in
        BugReportSharing.present(files, completed: finished)
    }

    init(backend: any GuestFilesBackend) {
        self.backend = backend
    }

    var browsing: Bool { !route.isEmpty }
    var current: String { route.last?.path ?? GuestPath.root }
    var running: Bool { transfer?.running == true }

    /// A new import or export has to wait.
    var busy: Bool { running || conflict != nil }

    // MARK: Folders

    func listing(_ folder: String) -> FolderListing? { listings[folder] }

    func entries(in folder: String) -> [GuestFile] {
        guard case let .loaded(files) = listings[folder] else { return [] }
        return files.sorted(by: order)
    }

    private func order(_ first: GuestFile, _ second: GuestFile) -> Bool {
        if first.isFolder != second.isFolder { return first.isFolder }
        switch sort {
        case .date where first.modified != second.modified:
            return first.modified > second.modified
        case .size where first.size != second.size:
            return first.size > second.size
        default:
            return first.name.localizedStandardCompare(second.name) == .orderedAscending
        }
    }

    func load(_ folder: String) async {
        // A folder that answers at once never shows a placeholder.
        let placeholder = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(150))
            guard !Task.isCancelled, let self, self.listings[folder] == nil else { return }
            self.listings[folder] = .loading
        }
        defer { placeholder.cancel() }
        do {
            listings[folder] = .loaded(try await backend.list(folder))
        } catch is CancellationError {
        } catch GuestFolderProblem.refused {
            listings[folder] = .refused
        } catch {
            listings[folder] = .failed(error.localizedDescription)
        }
        freeBytes = await backend.freeBytes()
    }

    func canImport(into folder: String) -> Bool {
        if case .loaded = listings[folder] { return true }
        return false
    }

    /// The trail: back to the folder at `index` of the route.
    func jump(to index: Int) {
        guard route.indices.contains(index), index < route.count - 1 else { return }
        route = Array(route.prefix(index + 1))
    }

    // MARK: Selection

    func isSelected(_ file: GuestFile) -> Bool { selection.contains { $0.path == file.path } }

    func toggle(_ file: GuestFile) {
        if isSelected(file) {
            selection.removeAll { $0.path == file.path }
        } else {
            selection.append(file)
        }
    }

    /// The files of the folder; its folders are chosen one by one.
    func selectAll(in folder: String) {
        for file in entries(in: folder) where !file.isFolder && !isSelected(file) {
            selection.append(file)
        }
    }

    func allSelected(in folder: String) -> Bool {
        let files = entries(in: folder).filter { !$0.isFolder }
        return !files.isEmpty && files.allSatisfy(isSelected)
    }

    func clearSelection() {
        selection = []
        reviewingSelection = false
    }

    /// "3 selected · 2.4 GiB"; a folder's size is known once it is exported.
    var selectionSummary: String {
        let bytes = selection.filter { !$0.isFolder }.reduce(0) { $0 + $1.size }
        let hasFolders = selection.contains(where: \.isFolder)
        guard bytes > 0, !hasFolders else {
            return "\(selection.count) selected"
        }
        return "\(selection.count) selected \u{00B7} \(Formats.size(bytes: bytes))"
    }

    // MARK: Import

    /// The picker's result, for the folder on screen.
    func received(_ picks: [FilePick]) {
        picking = false
        guard !picks.isEmpty, !busy else { return }
        let folder = current
        let names = Set(entries(in: folder).map(\.name))
        let clashing = picks.filter { names.contains($0.name) }
        guard clashing.isEmpty else {
            conflict = ImportConflict(picks: picks, clashing: clashing, folder: folder)
            return
        }
        startImport(picks.map { ($0, GuestPath.child($0.name, of: folder)) }, into: folder)
    }

    func resolveConflict(_ choice: ConflictChoice) {
        guard let conflict else { return }
        self.conflict = nil
        let clashing = Set(conflict.clashing.map(\.name))
        var taken = Set(entries(in: conflict.folder).map(\.name)).union(conflict.picks.map(\.name))
        var plan: [(FilePick, String)] = []
        for pick in conflict.picks {
            var name = pick.name
            if clashing.contains(name) {
                switch choice {
                case .skip: continue
                case .replace: break
                case .keepBoth:
                    name = Self.freeName(for: name, taken: taken)
                    taken.insert(name)
                }
            }
            var renamed = pick
            renamed.name = name
            plan.append((renamed, GuestPath.child(name, of: conflict.folder)))
        }
        if !plan.isEmpty { startImport(plan, into: conflict.folder) }
    }

    func cancelConflict() { conflict = nil }

    /// "Clip.mov" as "Clip (1).mov", the way Android names a second copy.
    static func freeName(for name: String, taken: Set<String>) -> String {
        let base = (name as NSString).deletingPathExtension
        let suffix = (name as NSString).pathExtension
        var count = 1
        while true {
            let candidate = suffix.isEmpty ? "\(base) (\(count))" : "\(base) (\(count)).\(suffix)"
            if !taken.contains(candidate) { return candidate }
            count += 1
        }
    }

    private func startImport(_ plan: [(pick: FilePick, path: String)], into folder: String) {
        let transfer = FileTransfer(
            direction: .importing, folder: folder,
            items: plan.map { FileTransfer.Item(name: $0.pick.name, size: $0.pick.size) }
        )
        replaceTransfer(with: transfer)
        runImport(transfer, plan: plan, retrying: Array(plan.indices))
    }

    private func runImport(_ transfer: FileTransfer, plan: [(pick: FilePick, path: String)], retrying indices: [Int]) {
        let backend = backend
        transfer.task = Task { [weak self] in
            for index in indices {
                guard !Task.isCancelled else { return }
                transfer.items[index].moved = 0
                transfer.items[index].state = .moving
                do {
                    try await backend.send(plan[index].pick, to: plan[index].path) { bytes in
                        Task { @MainActor in transfer.items[index].moved = bytes }
                    }
                    transfer.items[index].moved = transfer.items[index].size
                    transfer.items[index].state = .done
                } catch is CancellationError {
                    return
                } catch {
                    transfer.items[index].moved = 0
                    transfer.items[index].state = .failed(error.localizedDescription)
                }
            }
            self?.importEnded(transfer, plan: plan)
        }
    }

    private func importEnded(_ transfer: FileTransfer, plan: [(pick: FilePick, path: String)]) {
        let arrived = transfer.items.indices.filter { transfer.items[$0].state == .done }.map { plan[$0].path }
        fresh = Set(arrived)
        Task { await load(transfer.folder) }
        let failed = transfer.items.indices.filter { transfer.items[$0].state != .done }
        if failed.isEmpty {
            finish(transfer)
        } else {
            transfer.stage = .failed
            retry = { [weak self, weak transfer] in
                guard let self, let transfer else { return }
                transfer.stage = .running
                self.runImport(transfer, plan: plan, retrying: failed)
            }
        }
    }

    // MARK: Export

    /// Copies the selection out of the guest; the action menu takes it from
    /// there.
    func exportSelection() {
        export(selection)
        selection = []
        reviewingSelection = false
    }

    /// The long-press's Export: this one item, whatever is selected.
    func export(_ files: [GuestFile]) {
        guard !files.isEmpty, !busy else { return }
        let transfer = FileTransfer(
            direction: .exporting, folder: current,
            items: files.map { FileTransfer.Item(name: $0.name, size: $0.size, isFolder: $0.isFolder) }
        )
        transfer.measuring = files.contains(where: \.isFolder)
        replaceTransfer(with: transfer)
        runExport(transfer, files: files, retrying: Array(files.indices))
    }

    private func runExport(_ transfer: FileTransfer, files: [GuestFile], retrying indices: [Int]) {
        let backend = backend
        transfer.task = Task { [weak self] in
            guard let directory = try? Self.makeStagingDirectory() else {
                for index in indices {
                    transfer.items[index].state = .failed("Andarium couldn\u{2019}t make room for the copy.")
                }
                transfer.stage = .failed
                return
            }
            do {
                for index in indices where files[index].isFolder {
                    transfer.items[index].size = try await backend.measure(files[index])
                }
            } catch {
                return
            }
            transfer.measuring = false
            for index in indices {
                guard !Task.isCancelled else { return }
                transfer.items[index].moved = 0
                transfer.items[index].state = .moving
                do {
                    let url = try await backend.receive(files[index], into: directory) { bytes in
                        Task { @MainActor in transfer.items[index].moved = bytes }
                    }
                    transfer.staged.append(url)
                    transfer.items[index].moved = transfer.items[index].size
                    transfer.items[index].state = .done
                } catch is CancellationError {
                    return
                } catch {
                    transfer.items[index].moved = 0
                    transfer.items[index].state = .failed(error.localizedDescription)
                }
            }
            self?.exportEnded(transfer, files: files)
        }
    }

    private func exportEnded(_ transfer: FileTransfer, files: [GuestFile]) {
        let failed = transfer.items.indices.filter { transfer.items[$0].state != .done }
        guard failed.isEmpty else {
            transfer.stage = .failed
            retry = { [weak self, weak transfer] in
                guard let self, let transfer else { return }
                transfer.stage = .running
                self.runExport(transfer, files: files, retrying: failed)
            }
            return
        }
        transfer.stage = .ready
        share(transfer)
    }

    /// The action menu on a finished export. The copies wait until it has
    /// done something with them or the person closes the capsule.
    func share(_ transfer: FileTransfer) {
        guard transfer.stage == .ready else { return }
        _ = presentShare(transfer.staged) { [weak self, weak transfer] completed in
            guard completed, let self, let transfer else { return }
            Self.removeStaged(transfer)
            self.finish(transfer)
        }
    }

    private static func removeStaged(_ transfer: FileTransfer) {
        for file in transfer.staged { try? FileManager.default.removeItem(at: file) }
        transfer.staged = []
    }

    private static func makeStagingDirectory() throws -> URL {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("GuestFiles", isDirectory: true)
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    // MARK: Ending

    @ObservationIgnored private var retry: (() -> Void)?

    func retryFailed() { retry?() }

    /// A new transfer takes the place of one that ended: an export nobody
    /// shared gives up its staged copies.
    private func replaceTransfer(with new: FileTransfer) {
        if let old = transfer { Self.removeStaged(old) }
        retry = nil
        transfer = new
    }

    private func finish(_ transfer: FileTransfer) {
        transfer.stage = .finished
        guard !transfer.stays else { return }
        Task { [weak self, weak transfer] in
            try? await Task.sleep(for: .seconds(4))
            guard let self, let transfer, self.transfer === transfer else { return }
            self.transfer = nil
        }
    }

    /// Cancel, or the capsule's close: nothing partial stays behind.
    func closeTransfer() {
        guard let transfer else { return }
        transfer.task?.cancel()
        Self.removeStaged(transfer)
        self.transfer = nil
        retry = nil
        if transfer.direction == .importing { Task { await load(transfer.folder) } }
    }

    /// iOS stops the guest while Andarium is in the background. A transfer
    /// goes on when it is back, and says what happened for a moment.
    func cameBack() {
        guard let transfer, transfer.running else { return }
        transfer.pausedWhileAway = true
        Task { [weak transfer] in
            try? await Task.sleep(for: .seconds(5))
            transfer?.pausedWhileAway = false
        }
    }
}
