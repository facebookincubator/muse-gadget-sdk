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
import os

/* Logs go to stdout (xcrun devicectl device process launch --console shows
 * them, like adb logcat -s MuseCam) and to the unified log. */
private let osLog = Logger(subsystem: "com.musegadgets.test", category: "MuseCam")

func mlog(_ s: String) {
    print("MuseCam: " + s)
    osLog.notice("\(s, privacy: .public)")
}

/* Monotonic nanoseconds (the capture clock's base too). */
@inline(__always) func nowNs() -> Int64 {
    Int64(DispatchTime.now().uptimeNanoseconds)
}

@inline(__always) func nowMs() -> Int64 {
    nowNs() / 1_000_000
}

/*
 * Launch arguments, as Android's intent extras: xcrun devicectl device
 * process launch --device ID com.musegadgets.test -- -target A1B2C3
 * -auto YES -codec vp8 (UserDefaults reads "-key value" pairs). Remembered
 * settings live under other keys (Prefs), so an argument never shadows one.
 */
enum Extras {
    static func has(_ k: String) -> Bool {
        UserDefaults.standard.volatileDomain(forName: UserDefaults.argumentDomain)[k] != nil
    }

    static func string(_ k: String) -> String? {
        has(k) ? UserDefaults.standard.string(forKey: k) : nil
    }

    static func int(_ k: String, _ d: Int) -> Int {
        guard let s = string(k), let v = Int(s) else { return d }
        return v
    }

    static func bool(_ k: String, _ d: Bool) -> Bool {
        has(k) ? UserDefaults.standard.bool(forKey: k) : d
    }
}

enum Prefs {
    static let d = UserDefaults.standard

    static func int(_ k: String, _ def: Int = 0) -> Int {
        d.object(forKey: "pref." + k) == nil ? def : d.integer(forKey: "pref." + k)
    }

    static func setInt(_ k: String, _ v: Int) {
        d.set(v, forKey: "pref." + k)
    }

    static func string(_ k: String) -> String? {
        d.string(forKey: "pref." + k)
    }

    static func setString(_ k: String, _ v: String?) {
        d.set(v, forKey: "pref." + k)
    }

    static func bool(_ k: String) -> Bool {
        d.bool(forKey: "pref." + k)
    }

    static func setBool(_ k: String, _ v: Bool) {
        d.set(v, forKey: "pref." + k)
    }
}
