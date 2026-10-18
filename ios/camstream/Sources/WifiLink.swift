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
import Network

/*
 * Video over the local network (protocols/VIDEO.md, Wi-Fi): finds gadgets
 * with Bonjour (_musevideo._tcp), connects over TCP, and sends the frames
 * inside the gadget's credits (its buffer messages), control messages first.
 * A gadget can also be given by address (host or host:port), with no Bonjour.
 * The same flow control as Android's WifiLink.
 */
final class WifiLink: VideoLink {
    static let service = "_musevideo._tcp"
    static let defaultPort: UInt16 = 7766
    private static let json: UInt8 = 0x4A   /* 'J' */
    /* Largest write: smaller keeps a frame from holding the socket long when credits are short. */
    private static let chunk = 16384

    private weak var l: VideoLinkListener?
    private let q = DispatchQueue(label: "video-net")
    private var browser: NWBrowser?
    private var target: String?, targetAddr: String?
    private var closed = false, isReady = false, discovering = false, connectPaused = false
    private var conn: NWConnection?
    private var connected: String?   /* the remote address */
    private var connectedName: String?
    private let theCaps = Caps()
    /* Gadgets found: key ("svc:<name>") -> name and endpoint. */
    private var found: [String: (String, NWEndpoint)] = [:]

    /* Outgoing: control messages first, then frames within credits. */
    private let lock = NSCondition()
    private var controlQ: [Data] = []
    private var lastCtlMs: Int64 = 0
    private var frameQ: [Data] = []
    private var cur: Data?
    private var curOff = 0
    private var queued: Int64 = 0
    private var isStreaming = false
    private var sent: Int64 = 0                            /* stream bytes written since start */
    private var creditAvail: Int64 = 0, creditRecv: Int64 = 0   /* from the latest buffer message */
    private var bufferSize: Int64 = 65536
    private var maxAhead: Int64 = 16384
    private var rx = Data()
    private var connDone: DispatchSemaphore?

    init(listener: VideoLinkListener) {
        l = listener
    }

    override func ready() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return isReady
    }

    override func caps() -> Caps {
        theCaps
    }

    override func setTarget(address: String?, name: String?) {
        targetAddr = address
        target = name
    }

    override func describe() -> String {
        lock.lock()
        defer { lock.unlock() }
        return connected != nil ? "Wi-Fi " + connected! : "Wi-Fi, not connected"
    }

    /* ---- finding and connecting ---------------------------------------- */

    override func start() {
        closed = false
        let s = Thread { [weak self] in self?.sendLoop() }
        s.name = "video-send"
        s.qualityOfService = .userInteractive
        s.start()
        let c = Thread { [weak self] in self?.connectLoop() }
        c.name = "video-connect"
        c.start()
        discover()
    }

    override func close() {
        closed = true
        stopDiscovery()
        drop()
        lock.lock()
        lock.broadcast()
        lock.unlock()
    }

    private func discover() {
        q.async { [self] in
            if closed || discovering {
                return
            }
            discovering = true
            let p = NWParameters()
            p.includePeerToPeer = false
            let b = NWBrowser(for: .bonjour(type: Self.service, domain: nil), using: p)
            b.stateUpdateHandler = { st in
                if case .failed(let e) = st {
                    mlog("Bonjour browse failed: \(e)")
                }
            }
            b.browseResultsChangedHandler = { [weak self] results, _ in
                self?.onResults(results)
            }
            b.start(queue: q)
            browser = b
            mlog("looking for " + Self.service)
            if !connectPaused && connected == nil {
                l?.onStatus(target == nil && targetAddr == nil ? "Looking for a gadget on Wi-Fi..."
                                                               : "Looking for \(label()) on Wi-Fi...")
            }
        }
    }

    private func onResults(_ results: Set<NWBrowser.Result>) {
        var now: [String: (String, NWEndpoint)] = [:]
        for r in results {
            if case let .service(name, _, _, _) = r.endpoint {
                now["svc:" + name] = (name, r.endpoint)
            }
        }
        lock.lock()
        for (k, v) in now where found[k] == nil {
            mlog("found \(v.0)")
            found[k] = v
        }
        lock.broadcast()
        lock.unlock()
        for (k, v) in now {
            l?.onSeen(name: v.0, address: k, rssi: 0)
        }
    }

    private func stopDiscovery() {
        q.async { [self] in
            browser?.cancel()
            browser = nil
            discovering = false
        }
    }

    private func label() -> String {
        target ?? targetAddr ?? ""
    }

    /* What to connect to now: the address given, else the first gadget
     * found whose name contains the target (any gadget without one). Under lock. */
    private func pick() -> (String, NWEndpoint, String)? {
        if let a = targetAddr {
            if a.hasPrefix("svc:") {
                if let f = found[a] {
                    return (a, f.1, f.0)
                }
                return nil
            }
            var host = a, port = Self.defaultPort
            if let c = a.lastIndex(of: ":"), let p = UInt16(a[a.index(after: c)...]) {
                host = String(a[..<c])
                port = p
            }
            return (a, .hostPort(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port)!), target ?? host)
        }
        for (k, v) in found.sorted(by: { $0.key < $1.key }) where target == nil || v.0.contains(target!) {
            return (k, v.1, v.0)
        }
        return nil
    }

    private func connectLoop() {
        while !closed {
            lock.lock()
            let p = connectPaused || conn != nil ? nil : pick()
            if p == nil {
                _ = lock.wait(until: Date().addingTimeInterval(0.5))
                lock.unlock()
                continue
            }
            lock.unlock()
            let (key, ep, name) = p!
            l?.onStatus("Connecting to \(name)...")
            let tcp = NWProtocolTCP.Options()
            tcp.noDelay = true
            tcp.enableKeepalive = true
            tcp.connectionTimeout = 3
            let params = NWParameters(tls: nil, tcp: tcp)
            params.serviceClass = .interactiveVideo
            let c = NWConnection(to: ep, using: params)
            let up = DispatchSemaphore(value: 0), done = DispatchSemaphore(value: 0)
            var ok = false, signaledUp = false, signaledDone = false
            c.stateUpdateHandler = { st in
                switch st {
                case .ready:
                    ok = true
                    if !signaledUp { signaledUp = true; up.signal() }
                case .failed(let e):
                    mlog("connection to \(name): \(e)")
                    if !signaledUp { signaledUp = true; up.signal() }
                    if !signaledDone { signaledDone = true; done.signal() }
                case .waiting(let e):
                    mlog("connection to \(name) waiting: \(e)")
                    if !signaledUp { signaledUp = true; up.signal() }
                case .cancelled:
                    if !signaledUp { signaledUp = true; up.signal() }
                    if !signaledDone { signaledDone = true; done.signal() }
                default:
                    break
                }
            }
            c.start(queue: q)
            _ = up.wait(timeout: .now() + 4)
            if !ok {
                c.cancel()
                lock.lock()
                if key.hasPrefix("svc:") && targetAddr != key {
                    found.removeValue(forKey: key)   /* stale: Bonjour finds it again */
                }
                lock.unlock()
                Thread.sleep(forTimeInterval: 1)
                continue
            }
            var remote = name
            if let r = c.currentPath?.remoteEndpoint, case let .hostPort(h, port) = r {
                var hs = "\(h)"
                if let pct = hs.firstIndex(of: "%") {
                    hs = String(hs[..<pct])
                }
                remote = "\(hs):\(port)"
            }
            lock.lock()
            conn = c
            connected = remote
            connectedName = name
            rx.removeAll()
            connDone = done
            lock.unlock()
            stopDiscovery()
            receive(c)
            done.wait()
            let wasReady = ready()
            drop()
            if wasReady || !closed {
                l?.onDisconnected()
            }
            if !closed {
                Thread.sleep(forTimeInterval: 1)
                discover()
            }
        }
    }

    private func drop() {
        lock.lock()
        let c = conn
        conn = nil
        connected = nil
        isReady = false
        isStreaming = false
        frameQ.removeAll()
        controlQ.removeAll()
        cur = nil
        queued = 0
        lock.broadcast()
        lock.unlock()
        c?.cancel()
    }

    override func scanForPicker() {
        lock.lock()
        connectPaused = true
        let f = found
        lock.unlock()
        stopDiscovery()
        discover()
        for (k, v) in f {
            l?.onSeen(name: v.0, address: k, rssi: 0)
        }
    }

    override func pickerClosed() {
        lock.lock()
        connectPaused = false
        lock.broadcast()
        lock.unlock()
    }

    override func select(address: String, name: String) {
        lock.lock()
        connectPaused = false
        targetAddr = address
        target = name
        let c = conn
        let same = address == connected || (address.hasPrefix("svc:") && connectedName == name)
        lock.broadcast()
        lock.unlock()
        if !same {
            c?.cancel()   /* the connector drops it and finds the new one */
        }
    }

    override func connectedAddress() -> String? {
        lock.lock()
        defer { lock.unlock() }
        guard isReady, let n = connectedName else { return nil }
        return found.first { $0.value.0 == n }?.key ?? connected
    }

    /* ---- incoming ------------------------------------------------------ */

    private func receive(_ c: NWConnection) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, complete, err in
            guard let self else { return }
            if let d = data, !d.isEmpty {
                self.rx.append(d)
                if !self.parse() {
                    c.cancel()
                    return
                }
            }
            if complete || err != nil {
                if let e = err {
                    mlog("connection: \(e)")
                }
                c.cancel()
                return
            }
            self.receive(c)
        }
    }

    /* Whole messages out of rx; false: not the video protocol. */
    private func parse() -> Bool {
        while rx.count >= 3 {
            let b = rx.startIndex
            if rx[b] != Self.json {
                mlog("not the video protocol (byte \(rx[b]))")
                return false
            }
            let n = Int(rx[b + 1]) | Int(rx[b + 2]) << 8
            if rx.count < 3 + n {
                break
            }
            let body = rx.subdata(in: (b + 3)..<(b + 3 + n))
            rx.removeSubrange(b..<(b + 3 + n))
            if let o = try? JSONSerialization.jsonObject(with: body) as? [String: Any] {
                onMessage(o)
            } else {
                mlog("bad message from the gadget")
            }
        }
        return true
    }

    private static func i(_ o: [String: Any]?, _ k: String, _ d: Int = 0) -> Int {
        (o?[k] as? NSNumber)?.intValue ?? d
    }

    private func onMessage(_ m: [String: Any]) {
        switch m["type"] as? String ?? "" {
        case "hello":
            parseHello(m)
            lock.lock()
            bufferSize = theCaps.buffer
            isReady = true
            let name = connectedName ?? "gadget"
            lock.broadcast()
            lock.unlock()
            mlog("connected to \(name): \(theCaps)")
            l?.onStatus("Connected to \(name) over Wi-Fi")
            l?.onReady(theCaps)
        case "buffer":
            let avail = Int64(Self.i(m, "free")), recv = Int64(Self.i(m, "received"))
            lock.lock()
            creditAvail = avail
            creditRecv = recv
            lock.broadcast()
            lock.unlock()
            l?.onBufferUpdate(avail: avail, received: recv, shown: Self.i(m, "shown", -1),
                              dropped: Self.i(m, "dropped", -1), decodeMs: Self.i(m, "decode_ms", -1))
            if m["last_frame"] != nil {
                meter?.shown(Self.i(m, "last_frame"), ageMs: Self.i(m, "last_age_ms"), rxNs: nowNs())
            }
        case "keyframe":
            l?.onKeyframeRequest()
        case "want":
            l?.onRequestStreaming((m["on"] as? Bool) ?? false)
        case "stop":
            clearStream()
            l?.onStreamStopped()
        case "error":
            let f = m["for"] as? String ?? "", r = m["reason"] as? String ?? ""
            mlog("gadget refused \(f): \(r)")
            if f == "start" {
                clearStream()
                l?.onStartRefused(r)
            }
        default:
            break   /* a newer gadget's: ignored */
        }
    }

    private func parseHello(_ m: [String: Any]) {
        let c = theCaps
        if let d = m["display"] as? [String: Any] {
            c.dispW = Self.i(d, "width")
            c.dispH = Self.i(d, "height")
            c.round = d["round"] as? Bool ?? false
        }
        if let x = m["max"] as? [String: Any] {
            c.maxW = Self.i(x, "width", c.maxW)
            c.maxH = Self.i(x, "height", c.maxH)
            c.maxFps = Self.i(x, "fps", c.maxFps)
        }
        if let s = m["suggested"] as? [String: Any] {
            c.sugW = Self.i(s, "width", c.sugW)
            c.sugH = Self.i(s, "height", c.sugH)
            c.sugFps = Self.i(s, "fps", c.sugFps)
            c.sugKbps = Self.i(s, "kbps", c.sugKbps)
        }
        c.buffer = Int64(Self.i(m, "buffer", Int(c.buffer)))
        c.maxPlayoutMs = Self.i(m, "max_playout_ms")
        c.maxTiles = max(1, Self.i(m, "max_bands", 1))
        c.tileSplitPct = Self.i(m, "band_split_pct")
        c.vp8 = (m["codecs"] as? [String] ?? []).contains("vp8")
        c.clamp()
        c.video = true
    }

    private func clearStream() {
        lock.lock()
        isStreaming = false
        frameQ.removeAll()
        cur = nil
        queued = 0
        lock.unlock()
    }

    /* ---- outgoing ------------------------------------------------------ */

    private static func message(_ m: [String: Any]) -> Data {
        let t = (try? JSONSerialization.data(withJSONObject: m)) ?? Data()
        var b = Data([json, UInt8(t.count & 0xff), UInt8(t.count >> 8)])
        b.append(t)
        return b
    }

    override func startStreaming(w: Int, h: Int, fps: Int, kbps: Int, playoutMs: Int, bands: Int = 1, vp8: Bool = false) {
        let playout = min(playoutMs, theCaps.maxPlayoutMs)
        mlog("start \(vp8 ? "VP8" : "H.264") \(w)x\(h)@\(fps) \(kbps) kb/s, playout \(playout) ms"
             + (bands > 1 ? ", \(bands) bands" : "") + " (was streaming: \(streaming()))")
        var m: [String: Any] = ["type": "start", "codec": vp8 ? "vp8" : "h264", "width": w, "height": h, "fps": fps,
                                "kbps": kbps]
        if playout > 0 {
            m["playout_ms"] = playout
        }
        if bands > 1 {
            m["bands"] = bands
        }
        lock.lock()
        /* The device holds the playout delay's worth unread on top. */
        maxAhead = max(12288, Int64(kbps) * 1000 / 8 * Int64(150 + playout) / 1000)
        frameQ.removeAll()
        cur = nil
        queued = 0
        sent = 0
        creditAvail = 0   /* nothing until the device's first buffer message */
        creditRecv = 0
        isStreaming = true
        controlQ.append(Self.message(m))
        lock.broadcast()
        lock.unlock()
    }

    override func stopStreaming() {
        lock.lock()
        defer { lock.unlock() }
        if !isStreaming {
            return
        }
        isStreaming = false
        frameQ.removeAll()
        cur = nil
        queued = 0
        controlQ.append(Self.message(["type": "stop"]))
        lock.broadcast()
    }

    override func streaming() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return isStreaming
    }

    override func sendFrame(_ frame: Data) {
        lock.lock()
        defer { lock.unlock() }
        if !isStreaming {
            return
        }
        frameQ.append(frame)
        queued += Int64(frame.count)
        lock.broadcast()
    }

    override func queuedBytes() -> Int64 {
        lock.lock()
        defer { lock.unlock() }
        return queued
    }

    override func inFlight() -> Int64 {
        lock.lock()
        defer { lock.unlock() }
        return sent - creditRecv
    }

    private func sendLoop() {
        while !closed {
            var out: Data?
            var isCtl = false
            var frameDone = -1
            var c: NWConnection?
            lock.lock()
            while !closed {
                if !isReady || conn == nil {
                    _ = lock.wait(until: Date().addingTimeInterval(0.05))
                    continue
                }
                /* While not streaming, a status request every few seconds:
                 * the gadget turns away a new connection only while its
                 * client is talking (VIDEO.md, Trust). */
                if !isStreaming && nowMs() - lastCtlMs >= 4000 {
                    controlQ.append(Self.message(["type": "status"]))
                }
                if !controlQ.isEmpty {
                    out = controlQ.removeFirst()
                    isCtl = true
                    lastCtlMs = nowMs()
                    break
                }
                if isStreaming {
                    if cur == nil && !frameQ.isEmpty {
                        /* A new frame starts only while the device holds
                         * little: what's on its way plus what waits in its
                         * ring, unread. (A frame once started goes whole.) */
                        let backlog = sent - creditRecv + (bufferSize - creditAvail)
                        if backlog > (liveAhead > 0 ? liveAhead : maxAhead) {
                            let t0 = nowNs()
                            _ = lock.wait(until: Date().addingTimeInterval(0.01))
                            statGateUs += (nowNs() - t0) / 1000
                            continue
                        }
                        cur = frameQ.removeFirst()
                        curOff = 0
                    }
                    if let f = cur {
                        /* Inside the device's credits: anything past them
                         * would wait in the socket's buffers, pure latency. */
                        let room = creditAvail - (sent - creditRecv)
                        let k = Int(min(Int64(min(Self.chunk, f.count - curOff)), max(0, room)))
                        if k > 0 {
                            let s = f.startIndex + curOff
                            out = f.subdata(in: s..<(s + k))
                            curOff += k
                            sent += Int64(k)
                            queued -= Int64(k)
                            if curOff >= f.count {
                                frameDone = VideoFrame.number(f)
                                cur = nil
                            }
                            break
                        }
                        let t0 = nowNs()
                        _ = lock.wait(until: Date().addingTimeInterval(0.02))   /* out of credits */
                        statStallUs += (nowNs() - t0) / 1000
                        continue
                    }
                }
                _ = lock.wait(until: Date().addingTimeInterval(0.02))
            }
            c = conn
            lock.unlock()
            guard let data = out, let cn = c else {
                continue
            }
            let done = DispatchSemaphore(value: 0)
            var failed = false
            cn.send(content: data, completion: .contentProcessed { e in
                if let e {
                    mlog("write failed: \(e)")
                    failed = true
                }
                done.signal()
            })
            done.wait()
            if failed {
                cn.cancel()   /* the connector sees it and reconnects */
                continue
            }
            if !isCtl {
                lock.lock()
                statBytes += Int64(data.count)
                statWrites += 1
                lock.unlock()
            }
            if frameDone >= 0 {
                meter?.sent(frameDone, ns: nowNs())
            }
        }
    }
}
