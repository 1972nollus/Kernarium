// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
@preconcurrency import Security
import Testing
@testable import Andarium

/// The guest's adb host key as the Keychain keeps it.
struct AdbKeyStoreTests {
    private func store() -> AdbKeyStore {
        AdbKeyStore(tag: "com.imxieyi.andarium.adb-key.test-\(UUID().uuidString)")
    }

    @Test func aKeyIsMadeOnceFoundAgainAndNewAfterTheDelete() throws {
        let store = store()
        defer { store.delete() }
        #expect(try store.find() == nil)
        let first = try store.loadOrCreate()
        // 524 bytes of adb's structure, in base64.
        #expect(first.publicKeyLine.count == 700)
        #expect(Data(base64Encoded: first.publicKeyLine)?.count == 524)
        #expect(try store.loadOrCreate().publicKeyLine == first.publicKeyLine)
        #expect(store.delete())
        #expect(try store.find() == nil)
        #expect(try store.loadOrCreate().publicKeyLine != first.publicKeyLine)
        // Deleting what is not there is no failure.
        #expect(store.delete() && store.delete())
    }

    @Test func theKeyIsRSA2048ThisDeviceOnlyAndNotSynchronized() throws {
        let store = store()
        defer { store.delete() }
        _ = try store.loadOrCreate()
        let query: [String: Any] = [
            kSecClass as String: kSecClassKey,
            kSecAttrApplicationTag as String: Data(store.tag.utf8),
            kSecAttrKeyType as String: kSecAttrKeyTypeRSA,
            kSecAttrKeyClass as String: kSecAttrKeyClassPrivate,
            kSecAttrSynchronizable as String: false,
            kSecUseDataProtectionKeychain as String: true,
            kSecReturnAttributes as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        #expect(SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess)
        let attributes = try #require(item as? [String: Any])
        #expect(attributes[kSecAttrKeySizeInBits as String] as? Int == 2048)
        #expect(attributes[kSecAttrLabel as String] as? String == AdbKeyStore.label)
        #expect(attributes[kSecAttrAccessible as String] as? String
            == kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly as String)
        // 0 or false: not in iCloud Keychain.
        #expect((attributes[kSecAttrSynchronizable as String] as? Bool ?? false) == false)
    }

    @Test func theKeySignsATokenTheWayAdbdVerifiesIt() throws {
        let store = store()
        defer { store.delete() }
        let identity = try store.loadOrCreate()
        let token = Data((0..<20).map { UInt8($0 * 11 + 5) })
        let signature = try identity.signer.sign(token: token)
        let publicKey = try #require(SecKeyCopyPublicKey(identity.privateKey))
        #expect(SecKeyVerifySignature(
            publicKey, .rsaSignatureDigestPKCS1v15SHA1, token as CFData, signature as CFData, nil))
    }

    @Test func theLineGoesInTheBootRequestAsTheRuntimeAcceptsIt() throws {
        let store = store()
        defer { store.delete() }
        let line = try store.loadOrCreate().publicKeyLine
        // 1 through 1024 characters of A-Z a-z 0-9 + / =.
        #expect((1...1024).contains(line.count))
        #expect(line.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || "+/=".contains($0)) })
    }
}
