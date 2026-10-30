/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
import Foundation
import Security

/// Persists the identity and device tokens in the Keychain, readable after
/// first unlock so a reconnect can run while the screen is locked.
public final class KeychainStore {
    private let service: String

    public init(service: String = "musegadget") { self.service = service }

    public func load<T: Decodable>(_ type: T.Type, _ account: String) -> T? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data
        else { return nil }
        return try? JSONDecoder().decode(type, from: data)
    }

    @discardableResult
    public func save<T: Encodable>(_ value: T, _ account: String) -> Bool {
        guard let data = try? JSONEncoder().encode(value) else { return false }
        delete(account)
        let attributes: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
            kSecValueData as String: data,
        ]
        return SecItemAdd(attributes as CFDictionary, nil) == errSecSuccess
    }

    public func delete(_ account: String) {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        SecItemDelete(query as CFDictionary)
    }
}

/// Keeps a paired gadget connected to its Muse (`linux/src/musegadget/service.py`):
/// fetch VMs, connect to the default one, serve commands, back off, rotate
/// the device token every three hours or when the API rejects it.
public final class GadgetService {
    public static let identityAccount = "identity"
    public static let credentialsAccount = "pairing"
    static let tokenRefreshAge: TimeInterval = 3 * 3600

    public let store: KeychainStore
    public let identity: GadgetIdentity
    public var device: DeviceDescription
    public var runCommand: LinkSession.CommandHandler
    public var log: (String) -> Void = { print("[musegadget]", $0) }
    /// Called when the Muse removed the pairing or revoked its token.
    public var onUnpaired: () -> Void = {}
    private var loop: Task<Void, Never>?

    // Shared between the caller and the service task.
    private let stateLock = NSLock()
    private var currentValue: LinkSession?
    private var sdkTokenValue: String?
    private var sdkTokenUnreported = false

    public var current: LinkSession? { locked { currentValue } }

    /// Reported with the next token refresh after it changes.
    public var sdkToken: String? {
        get { locked { sdkTokenValue } }
        set { locked { sdkTokenValue = newValue; sdkTokenUnreported = newValue != nil } }
    }

    public init(store: KeychainStore = KeychainStore(), device: (GadgetIdentity) -> DeviceDescription,
                sdkToken: String?, runCommand: @escaping LinkSession.CommandHandler) {
        self.store = store
        if let saved = store.load(GadgetIdentity.self, Self.identityAccount) {
            identity = saved
        } else {
            identity = GadgetIdentity.generate()
            store.save(identity, Self.identityAccount)
        }
        self.device = device(identity)
        self.runCommand = runCommand
        self.sdkToken = sdkToken
    }

    public var credentials: DeviceCredentials? {
        store.load(DeviceCredentials.self, Self.credentialsAccount)
    }

    /// The `provision` step of setup: check the tokens work, then save them.
    public func verifyAndSave(_ credentials: DeviceCredentials, commit: (() -> Bool) -> Bool) async throws {
        let (vms, status) = await MuseAPI(apiURLv2: credentials.apiURLv2).fetchVMs(accessToken: credentials.accessToken)
        guard !vms.isEmpty else {
            log("device token check failed (HTTP \(status.map(String.init) ?? "-"))")
            throw ProvisionFailed("auth_failed")
        }
        guard commit({ locked { store.save(credentials, Self.credentialsAccount) } }) else {
            throw ProvisionFailed("error_storage")
        }
    }

    public func start() {
        guard loop == nil else { return }
        loop = Task { await run() }
    }

    /// Stops the service and ends the current session.
    public func stop() {
        loop?.cancel()
        loop = nil
        current?.close()
    }

    /// Starts over with the pairing just saved, dropping any older session.
    public func restart() {
        stop()
        start()
    }

    private func run() async {
        var failures = 0
        while !Task.isCancelled {
            guard var creds = credentials else {
                log("not paired")
                try? await Task.sleep(nanoseconds: 30_000_000_000)
                continue
            }
            let api = MuseAPI(apiURLv2: creds.apiURLv2)
            let due = Date().timeIntervalSince(creds.accessTokenSavedAt) >= Self.tokenRefreshAge
            let reportToken = locked { () -> Bool in
                defer { sdkTokenUnreported = false }
                return sdkTokenUnreported
            }
            if due || reportToken {
                if let rotated = await refresh(creds, api: api, mayRevoke: due) { creds = rotated }
                if Task.isCancelled { return }
                if credentials == nil { continue }  // revoked
            }
            let (vms, status) = await api.fetchVMs(accessToken: creds.accessToken)
            if status == 401 {
                _ = await refresh(creds, api: api, mayRevoke: true)
                try? await Task.sleep(nanoseconds: 15_000_000_000)
                continue
            }
            guard let vm = vms.first(where: \.isDefault) ?? vms.first else {
                await backoff(&failures)
                continue
            }
            let socket = URLSessionGadgetWebSocket(
                url: LinkSession.noiseURL(host: creds.noiseHost.isEmpty ? MuseAPI.defaultNoiseHost : creds.noiseHost,
                                          vmID: vm.id.isEmpty ? vm.name : vm.id),
                bearer: vm.authToken, userAgent: api.userAgent)
            let session = LinkSession(socket: socket, device: device, runCommand: runCommand)
            session.log = log
            locked { currentValue = session }
            if Task.isCancelled { session.close() }  // stopped while connecting
            log("connecting to \(vm.name.isEmpty ? vm.id : vm.name)")
            let outcome = await session.run()
            locked { if currentValue === session { currentValue = nil } }
            log("session ended: \(outcome)")
            if Task.isCancelled { return }
            switch outcome {
            case .stopped: return
            case .unpaired:
                removeCredentials(ifStill: creds)
                continue
            default:
                if let registered = session.registeredAt, Date().timeIntervalSince(registered) >= 30 { failures = 0 }
                await backoff(&failures, floor: outcome == .closed ? 0 : 15)
            }
        }
    }

    /// A 401 removes the pairing only when the current token is known bad or
    /// expired, never when the refresh was just to report the SDK token.
    private func refresh(_ creds: DeviceCredentials, api: MuseAPI, mayRevoke: Bool) async -> DeviceCredentials? {
        let (tokens, status) = await api.refreshDeviceToken(
            refreshToken: creds.refreshToken, nodeID: identity.nodeID, sdkToken: sdkToken)
        guard let tokens else {
            if status == 401, mayRevoke {
                log("pairing revoked; pair again")
                removeCredentials(ifStill: creds)
            }
            return nil
        }
        var rotated = creds
        rotated.accessToken = tokens.access
        rotated.refreshToken = tokens.refresh
        rotated.accessTokenSavedAt = Date()
        // A newer pairing saved meanwhile wins over this rotation.
        guard replaceCredentials(creds, with: rotated) else { return nil }
        log("device token rotated")
        return rotated
    }

    /// Deletes the pairing only if it is still the one this loop used, so an
    /// old session can't remove a pairing made since.
    func removeCredentials(ifStill creds: DeviceCredentials) {
        let removed = locked { () -> Bool in
            guard credentials?.refreshToken == creds.refreshToken else { return false }
            store.delete(Self.credentialsAccount)
            return true
        }
        if removed { onUnpaired() }
    }

    func replaceCredentials(_ old: DeviceCredentials, with new: DeviceCredentials) -> Bool {
        locked {
            guard credentials?.refreshToken == old.refreshToken else { return false }
            return store.save(new, Self.credentialsAccount)
        }
    }

    private func locked<T>(_ body: () -> T) -> T {
        stateLock.lock(); defer { stateLock.unlock() }
        return body()
    }

    private func backoff(_ failures: inout Int, floor: Double = 0) async {
        let delay = max(min(2 * pow(2, Double(failures)), 60), floor)
        failures += 1
        log("reconnecting in \(Int(delay))s")
        try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
    }
}

/// A pairing done on another machine (e.g. `MusePairMac`), to be imported
/// by the gadget that will use it. The tokens belong to the identity's node
/// id, not to any hardware.
public struct PairingExport: Codable {
    public var identity: GadgetIdentity
    public var credentials: DeviceCredentials

    public init(identity: GadgetIdentity, credentials: DeviceCredentials) {
        self.identity = identity
        self.credentials = credentials
    }

    public static let fileName = "muse-import.json"

    public struct ImportFailed: Error {}

    /// Moves an exported pairing from `directory` into the Keychain, replacing
    /// this gadget's identity. Returns the imported identity, if any. If the
    /// Keychain refuses either record, the previous pairing is put back and
    /// the file is kept for the next launch.
    @discardableResult
    public static func importIfPresent(from directory: URL, into store: KeychainStore) throws -> GadgetIdentity? {
        let url = directory.appendingPathComponent(fileName)
        guard let data = try? Data(contentsOf: url),
              let export = try? JSONDecoder().decode(PairingExport.self, from: data)
        else { return nil }
        let oldIdentity = store.load(GadgetIdentity.self, GadgetService.identityAccount)
        let oldCredentials = store.load(DeviceCredentials.self, GadgetService.credentialsAccount)
        guard store.save(export.identity, GadgetService.identityAccount),
              store.save(export.credentials, GadgetService.credentialsAccount)
        else {
            restore(oldIdentity, GadgetService.identityAccount, in: store)
            restore(oldCredentials, GadgetService.credentialsAccount, in: store)
            throw ImportFailed()
        }
        try? FileManager.default.removeItem(at: url)
        return export.identity
    }

    private static func restore<T: Encodable>(_ value: T?, _ account: String, in store: KeychainStore) {
        if let value { store.save(value, account) } else { store.delete(account) }
    }
}
