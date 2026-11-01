// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The system Andarium carries, as the Info screen and the boot screens name
/// it. The live app reads it from the release manifest the packaging step
/// stages beside the system image.
public struct SystemInfo: Equatable, Sendable {
    public var distribution: String
    public var android: String
    /// The release identifier, which names this build everywhere.
    public var release: String
    /// Android's own build identifier.
    public var buildID: String?
    /// Not in the release manifest; shown only where known.
    public var securityPatch: String?
    public var kernel: String
    public var architecture: String
    public var appVersion: String

    public init(
        distribution: String, android: String, release: String, buildID: String? = nil,
        securityPatch: String? = nil, kernel: String, architecture: String, appVersion: String
    ) {
        self.distribution = distribution
        self.android = android
        self.release = release
        self.buildID = buildID
        self.securityPatch = securityPatch
        self.kernel = kernel
        self.architecture = architecture
        self.appVersion = appVersion
    }

    public enum ManifestError: Error, Equatable {
        case malformed
        case missing(String)
    }

    /// Reads a `release-manifest.json`.
    public init(manifest: Data, appVersion: String) throws {
        guard let root = try? JSONSerialization.jsonObject(with: manifest) as? [String: Any] else {
            throw ManifestError.malformed
        }
        func string(_ object: [String: Any]?, _ key: String, _ path: String) throws -> String {
            guard let value = object?[key] as? String, !value.isEmpty else { throw ManifestError.missing(path) }
            return value
        }
        let android = root["android"] as? [String: Any]
        let kernel = root["kernel"] as? [String: Any]
        let family = try string(root, "product_family", "product_family")
        let release = try string(root, "release_id", "release_id")
        guard let major = android?["major"] as? Int else { throw ManifestError.missing("android.major") }
        let kernelRelease = try string(kernel, "release", "kernel.release")
        guard let pageSize = kernel?["page_size"] as? Int, pageSize > 0 else {
            throw ManifestError.missing("kernel.page_size")
        }

        // "lineage-24-mainline-6.18-4k" names LineageOS 24.
        let parts = family.split(separator: "-")
        guard parts.count >= 2, parts[0] == "lineage", Int(parts[1]) != nil else {
            throw ManifestError.missing("product_family")
        }
        // "6.18.54-g4bcc3653b86a" is Linux 6.18.54.
        let version = kernelRelease.split(separator: "-", maxSplits: 1).first.map(String.init) ?? kernelRelease

        self.init(
            distribution: "LineageOS \(parts[1])", android: String(major), release: release,
            buildID: android?["build_id"] as? String, securityPatch: nil,
            kernel: "Linux \(version)", architecture: "arm64, \(pageSize / 1024) KB pages",
            appVersion: appVersion
        )
    }
}
