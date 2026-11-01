import AndariumCore
import Foundation

/// Writes the guest's disk from the seed: a zstd archive decoded sparse, or
/// a raw image cloned, then the image grown to the guest's size. The decode
/// is the tens of seconds a Create or Erase takes.
nonisolated struct LiveSeedWriter: SeedWriter {
    func write(
        seed: SeedImage, to destination: URL, imageGiB: Int,
        progress: @escaping @Sendable (Double) -> Void
    ) async throws {
        progress(0)
        try await Task.detached(priority: .userInitiated) {
            if seed.url.pathExtension == "zst" {
                try ZstdUtil.decompress(from: seed.url, to: destination, sparse: true) { step in
                    progress(Double(step.compressedBytesConsumed) / Double(max(step.compressedBytesTotal, 1)) * 0.95)
                }
            } else {
                try? FileManager.default.removeItem(at: destination)
                try FileManager.default.copyItem(at: seed.url, to: destination)
                progress(0.95)
            }
        }.value
        try await MainActor.run {
            _ = try DiskUtil().resizeAndroidImage(at: destination, toGiB: imageGiB)
        }
        progress(1)
    }

    func grow(image: URL, toGiB imageGiB: Int) async throws {
        // The disk utility belongs to the main actor with the rest of the
        // host code; growing is a clone and a table edit, not a copy.
        try await MainActor.run {
            _ = try DiskUtil().resizeAndroidImage(at: image, toGiB: imageGiB)
        }
    }
}
