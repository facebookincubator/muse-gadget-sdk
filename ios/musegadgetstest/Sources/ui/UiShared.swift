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

/*
 * What the app and its broadcast extension share. The wildcard profiles this
 * app is signed with have no app groups, so there's no shared container or
 * defaults; a keychain access group works with any profile whose
 * keychain-access-groups allow it (build.sh adds "<prefix>.musegadgets.shared"
 * to both), and Darwin notifications carry the start/stop signals.
 */
struct MirrorSettings: Codable, Equatable {
    var cx = 0.5, cy = 0.5   /* the window's center, 0..1 of the screen's width and height (portrait) */
    var size = 0.9            /* its side, 0..1 of the screen's width */
    var warp = true
    var gadget = ""           /* the gadget's peripheral identifier (CoreBluetooth) */
    var gadgetName = ""
    var x264 = true
    var hid = false           /* send hid_map (relative mouse) for the window */
    var hidCountsW = 0, hidCountsH = 0
    var sharp: Bool? = true    /* quality: sharp (still patches) or fast; optional for older saved settings */
    /* How far the window may go past the screen's edges (mg_ui_control_overscroll):
     * 0 none, 1 vertical (past the top and bottom by half its height), 2 all. */
    var overscroll: Int? = 1
    /* Pacing (UiStreamer): the fast rate for small changes (nil: 30; 15 or less: off) and the old rule (scripts: -uifast, -uilegacy). */
    var fastFps: Int? = nil
    var legacyPacing: Bool? = nil
    static let overscrollNames = ["None", "Vertical", "All"]

    /* The window as a crop (x, y, w, h in 0..1) of a w x h picture of the screen,
     * square in pixels. It may reach past the screen's edges by up to half its
     * size (so the screen's edge can sit at the panel's center): black beyond. */
    func crop(w: Int, h: Int) -> SIMD4<Float> {
        let W = Double(w), H = Double(h)
        let side = min(min(W, H), max(16, size * W))
        let os = overscroll ?? 1
        let mx = os >= 2 ? side / 2 : 0, my = os >= 1 ? side / 2 : 0   /* how far past each edge */
        let x0 = min(max(-mx, cx * W - side / 2), W - side + mx)
        let y0 = min(max(-my, cy * H - side / 2), H - side + my)
        return SIMD4<Float>(Float(x0 / W), Float(y0 / H), Float(side / W), Float(side / H))
    }

    /* The window's center within crop's limits on a w x h screen (so a drag past
     * a limit doesn't pile up), and back to the middle. */
    mutating func clampCenter(w: Double, h: Double) {
        let c = crop(w: Int(w), h: Int(h))
        cx = Double(c.x + c.z / 2)
        cy = Double(c.y + c.w / 2)
    }

    mutating func recenter() {
        cx = 0.5
        cy = 0.5
    }
}

/* The extension's report, for the app's UI tab. */
struct MirrorStatus: Codable {
    var at = 0.0                 /* time of the report (seconds since 1970) */
    var state = ""
    var ended = false            /* the broadcast is over; state says why */
    var frames = 0, changes = 0, refines = 0, kbps = 0.0
    var memMB = 0.0, cpuPct = 0.0
    var latency = ""
    var bluetooth = ""
}

enum UiShared {
    static let service = "musegadgets.mirror"
    static let notifyStarted = "com.musegadgets.mirror.started"
    static let notifyStopped = "com.musegadgets.mirror.stopped"
    static let notifyStop = "com.musegadgets.mirror.stop"        /* app -> extension: finish the broadcast */
    static let notifySettings = "com.musegadgets.mirror.settings" /* app -> extension: read the settings again */
    static let notifyProofKey = "com.musegadgets.mirror.proofkey" /* a proof key changed or was removed */

    /* The app ID prefix this binary was signed with (its default keychain group's first part). */
    static let prefix: String? = {
        let q: [CFString: Any] = [kSecClass: kSecClassGenericPassword, kSecAttrService: "musegadgets.probe",
                                  kSecAttrAccount: "probe", kSecValueData: Data([1]),
                                  kSecAttrAccessible: kSecAttrAccessibleAfterFirstUnlock]
        SecItemAdd(q as CFDictionary, nil)
        let r: [CFString: Any] = [kSecClass: kSecClassGenericPassword, kSecAttrService: "musegadgets.probe",
                                  kSecAttrAccount: "probe", kSecReturnAttributes: true]
        var out: CFTypeRef?
        guard SecItemCopyMatching(r as CFDictionary, &out) == errSecSuccess, let a = out as? [CFString: Any],
              let g = a[kSecAttrAccessGroup] as? String, let dot = g.firstIndex(of: ".") else { return nil }
        return String(g[..<dot])
    }()

    static var group: String? { prefix.map { $0 + ".musegadgets.shared" } }

    static func save<T: Encodable>(_ key: String, _ v: T) {
        guard let g = group, let data = try? JSONEncoder().encode(v) else { return }
        let q: [CFString: Any] = [kSecClass: kSecClassGenericPassword, kSecAttrService: service, kSecAttrAccount: key,
                                  kSecAttrAccessGroup: g]
        let upd: [CFString: Any] = [kSecValueData: data]
        var st = SecItemUpdate(q as CFDictionary, upd as CFDictionary)
        if st == errSecItemNotFound {
            var add = q
            add[kSecValueData] = data
            add[kSecAttrAccessible] = kSecAttrAccessibleAfterFirstUnlock
            st = SecItemAdd(add as CFDictionary, nil)
        }
        if st != errSecSuccess {
            mlog("shared keychain: save \(key) failed (\(st)) in group \(g)")
        }
    }

    static func load<T: Decodable>(_ key: String, _ type: T.Type) -> T? {
        guard let g = group else { return nil }
        let q: [CFString: Any] = [kSecClass: kSecClassGenericPassword, kSecAttrService: service, kSecAttrAccount: key,
                                  kSecAttrAccessGroup: g, kSecReturnData: true]
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let d = out as? Data else { return nil }
        return try? JSONDecoder().decode(type, from: d)
    }

    /* Token proof keys (TokenProof), one per gadget by its full name, for the app and the extension. */
    static func proofKey(for gadget: String) -> [UInt8]? {
        load("proof:" + gadget, String.self).flatMap(TokenProof.parseKey)
    }

    /* This device only (never in a backup): a matching proof also lets a client erase the gadget's tokens. */
    static func setProofKey(_ k: [UInt8]?, for gadget: String) {
        guard let g = group else { return }
        let q: [CFString: Any] = [kSecClass: kSecClassGenericPassword, kSecAttrService: service,
                                  kSecAttrAccount: "proof:" + gadget, kSecAttrAccessGroup: g]
        defer { post(notifyProofKey) }   /* a running broadcast reads it again */
        guard let k, let data = try? JSONEncoder().encode(k.map { String(format: "%02x", $0) }.joined()) else {
            SecItemDelete(q as CFDictionary)
            return
        }
        /* In place: no moment without a key that a reconnect could read. */
        let upd: [CFString: Any] = [kSecValueData: data, kSecAttrAccessible: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly]
        var st = SecItemUpdate(q as CFDictionary, upd as CFDictionary)
        if st == errSecItemNotFound {
            var add = q
            add[kSecValueData] = data
            add[kSecAttrAccessible] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
            st = SecItemAdd(add as CFDictionary, nil)
        }
        if st != errSecSuccess {
            mlog("shared keychain: saving a proof key failed (\(st)) in group \(g)")
        }
    }

    static func post(_ name: String) {
        CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(), CFNotificationName(name as CFString),
                                             nil, nil, true)
    }

    private static var handlers: [String: () -> Void] = [:]

    /* Calls f (on the main queue) on each Darwin notification called name. */
    static func observe(_ name: String, _ f: @escaping () -> Void) {
        handlers[name] = f
        CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), nil, { _, _, n, _, _ in
            guard let s = n?.rawValue as String? else { return }
            DispatchQueue.main.async { UiShared.handlers[s]?() }
        }, name as CFString, nil, .deliverImmediately)
    }
}

/* This process's memory footprint (what the extension's 50 MB limit counts) and CPU time. */
enum ProcStats {
    static func footprintMB() -> Double {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let kr = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count) }
        }
        return kr == KERN_SUCCESS ? Double(info.phys_footprint) / 1_048_576 : -1
    }

    /* CPU seconds this process has used. */
    static func cpuSeconds() -> Double {
        var u = rusage()
        getrusage(RUSAGE_SELF, &u)
        return Double(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + Double(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6
    }
}
