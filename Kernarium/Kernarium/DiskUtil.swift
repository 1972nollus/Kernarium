import Foundation
import KernariumGPTFDisk

protocol DiskCapacityManaging {
    func resizeAndroidImage(at image: URL, toGiB capacity: Int) throws -> Bool
}

/// Host-side operations on raw guest disk images. This layer changes only the
/// file capacity and, for Android, its GPT. The guest grows only a filesystem
/// whose containing partition is already larger when the kernel discovers it.
final class DiskUtil: DiskCapacityManaging {
    static let bytesPerGiB: UInt64 = 1024 * 1024 * 1024
    static let androidUserdataPartition: UInt32 = 10
    static let androidTrailingFreeBytes: UInt64 = 1024 * 1024

    enum ResizeError: Error, LocalizedError {
        case invalidCapacity(Int)
        case unsafeImage(URL)
        case staleResizeFile(URL)
        case cannotShrink(current: UInt64, requested: UInt64)
        case sizeMismatch(requested: UInt64, actual: UInt64)
        case gptfdisk(operation: String, code: Int32, message: String)
        case gptLayoutMismatch

        var errorDescription: String? {
            switch self {
            case let .invalidCapacity(capacity):
                "invalid disk capacity: \(capacity) GiB"
            case let .unsafeImage(url):
                "disk image is not a regular file: \(url.path)"
            case let .staleResizeFile(url):
                "stale disk resize file must be removed: \(url.path)"
            case let .cannotShrink(current, requested):
                "disk image cannot shrink from \(current) to \(requested) bytes"
            case let .sizeMismatch(requested, actual):
                "disk image resize requested \(requested) bytes but produced " +
                    "\(actual) bytes"
            case let .gptfdisk(operation, code, message):
                "GPT fdisk \(operation) failed (\(code)): \(message)"
            case .gptLayoutMismatch:
                "GPT fdisk did not produce the requested Android disk layout"
            }
        }
    }

    func imageSize(at image: URL) throws -> UInt64 {
        let values = try image.resourceValues(forKeys: [
            .isRegularFileKey,
            .isSymbolicLinkKey,
        ])
        guard values.isRegularFile == true,
              values.isSymbolicLink != true
        else {
            throw ResizeError.unsafeImage(image)
        }
        let handle = try FileHandle(forReadingFrom: image)
        do {
            let size = try handle.seekToEnd()
            try handle.close()
            return size
        } catch {
            try? handle.close()
            throw error
        }
    }

    static func byteCount(forGiB capacity: Int) throws -> UInt64 {
        guard capacity > 0,
              let value = UInt64(exactly: capacity),
              value <= UInt64.max / bytesPerGiB
        else {
            throw ResizeError.invalidCapacity(capacity)
        }
        return value * bytesPerGiB
    }

    func androidImageIsPrepared(
        at image: URL,
        forGiB capacity: Int
    ) throws -> Bool {
        let requested = try Self.byteCount(forGiB: capacity)
        guard try imageSize(at: image) == requested else {
            return false
        }
        let info = try readAndroidPartition(at: image)
        guard info.block_size != 0,
              Self.androidTrailingFreeBytes % UInt64(info.block_size) == 0
        else {
            throw ResizeError.gptLayoutMismatch
        }
        let expectedDiskLBAs = requested / UInt64(info.block_size)
        let trailingLBAs = Self.androidTrailingFreeBytes /
            UInt64(info.block_size)
        guard expectedDiskLBAs > trailingLBAs else {
            throw ResizeError.invalidCapacity(capacity)
        }
        return info.disk_lbas == expectedDiskLBAs &&
            info.backup_header_lba + 1 == expectedDiskLBAs &&
            info.last_lba == expectedDiskLBAs - trailingLBAs - 1
    }

    /// Expands a regular image sparsely. Shrinking is deliberately rejected
    /// because it can discard guest data.
    @discardableResult
    func resizeImage(at image: URL, toGiB capacity: Int) throws -> Bool {
        let requested = try Self.byteCount(forGiB: capacity)
        let current = try imageSize(at: image)
        if current == requested {
            return false
        }
        guard current < requested else {
            throw ResizeError.cannotShrink(
                current: current,
                requested: requested
            )
        }

        let handle = try FileHandle(forWritingTo: image)
        do {
            try handle.truncate(atOffset: requested)
            try handle.synchronize()
            try handle.close()
        } catch {
            try? handle.close()
            throw error
        }
        let actual = try imageSize(at: image)
        guard actual == requested else {
            throw ResizeError.sizeMismatch(requested: requested, actual: actual)
        }
        return true
    }

    /// Expands the raw image and then grows Android's final userdata GPT entry.
    /// GPT mutation is host-owned so the guest never changes a live partition
    /// table while its block device is in use.
    @discardableResult
    func resizeAndroidImage(at image: URL, toGiB capacity: Int) throws -> Bool {
        let requested = try Self.byteCount(forGiB: capacity)
        let current = try imageSize(at: image)
        guard current <= requested else {
            throw ResizeError.cannotShrink(
                current: current,
                requested: requested
            )
        }

        let before = try readAndroidPartition(at: image)
        guard before.block_size != 0,
              Self.androidTrailingFreeBytes % UInt64(before.block_size) == 0
        else {
            throw ResizeError.gptLayoutMismatch
        }
        let expectedDiskLBAs = requested / UInt64(before.block_size)
        guard expectedDiskLBAs >
            Self.androidTrailingFreeBytes / UInt64(before.block_size)
        else {
            throw ResizeError.invalidCapacity(capacity)
        }
        let expectedLastLBA = expectedDiskLBAs -
            (Self.androidTrailingFreeBytes / UInt64(before.block_size)) - 1
        let gptNeedsResize = before.disk_lbas != expectedDiskLBAs ||
            before.backup_header_lba + 1 != expectedDiskLBAs ||
            before.last_lba != expectedLastLBA
        guard current != requested || gptNeedsResize else {
            return false
        }

        let staging = image.appendingPathExtension("resize.tmp")
        let fileManager = FileManager.default
        guard !fileManager.fileExists(atPath: staging.path) else {
            throw ResizeError.staleResizeFile(staging)
        }
        try fileManager.copyItem(at: image, to: staging)
        defer { try? fileManager.removeItem(at: staging) }
        _ = try resizeImage(at: staging, toGiB: capacity)

        var info = kernarium_gptfdisk_partition_info()
        try callGPTFDisk(operation: "resize", image: staging) {
            path, error, errorSize in
            kernarium_gptfdisk_grow_partition_to_fill(
                path,
                Self.androidUserdataPartition,
                Self.androidTrailingFreeBytes,
                &info,
                error,
                errorSize
            )
        }
        guard info.block_size != 0,
              info.disk_lbas == requested / UInt64(info.block_size),
              info.backup_header_lba + 1 == info.disk_lbas,
              info.last_lba == info.disk_lbas -
                (Self.androidTrailingFreeBytes / UInt64(info.block_size)) - 1
        else {
            throw ResizeError.gptLayoutMismatch
        }
        _ = try fileManager.replaceItemAt(image, withItemAt: staging)
        return true
    }

    private func readAndroidPartition(
        at image: URL
    ) throws -> kernarium_gptfdisk_partition_info {
        var info = kernarium_gptfdisk_partition_info()
        try callGPTFDisk(operation: "inspect", image: image) {
            path, error, errorSize in
            kernarium_gptfdisk_read_partition(
                path,
                Self.androidUserdataPartition,
                &info,
                error,
                errorSize
            )
        }
        return info
    }

    private func callGPTFDisk(
        operation: String,
        image: URL,
        _ body: (
            UnsafePointer<CChar>,
            UnsafeMutablePointer<CChar>,
            Int
        ) -> Int32
    ) throws {
        var error = [CChar](repeating: 0, count: 512)
        let result = image.path.withCString { path in
            error.withUnsafeMutableBufferPointer { buffer in
                body(path, buffer.baseAddress!, buffer.count)
            }
        }
        guard result == KERNARIUM_GPTFDISK_OK else {
            throw ResizeError.gptfdisk(
                operation: operation,
                code: result,
                message: String(cString: error)
            )
        }
    }
}
