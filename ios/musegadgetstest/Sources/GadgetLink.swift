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
import CoreBluetooth
import Foundation

/* musegadgets BLE (protocols/mgcommands.h): the UUIDs and numbers the app uses, as Android's Mg.java. */
enum Mg {
    static let service = CBUUID(string: "65E6635D-174A-4AF0-805E-D1EDED7C2A63")
    static let control = CBUUID(string: "732CE4AB-9F36-4F11-9147-0BAC4FCE97C4")
    static let data = CBUUID(string: "5B84AFDF-3B31-4FEF-89E6-514A4A97ABB3")
    static let nusService = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    static let nusTx = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")

    static let cmdChangeDataType: UInt8 = 8
    static let cmdRequestStatus: UInt8 = 10
    static let cmdSupportedFeatures: UInt8 = 16
    static let cmdError: UInt8 = 25
    static let cmdStreamAudio: UInt8 = 29
    static let cmdStreamVideo: UInt8 = 0x84
    static let cmdStartMic: UInt8 = 1, cmdStopMic: UInt8 = 2, cmdGesture: UInt8 = 11
    static let cmdAssistantState: UInt8 = 0x83
    static let settingPtt: UInt8 = 2
    static let dis = CBUUID(string: "180A"), disModel = CBUUID(string: "2A24")

    static let typeVideoH264: UInt8 = 24
    static let typeVideoVp8: UInt8 = 25
    static let typeSbc: UInt8 = 1
    static let typeLc3: UInt8 = 19

    static let vRequestCapabilities: UInt8 = 0
    static let vReportCapabilities: UInt8 = 1
    static let vStartStreaming: UInt8 = 2
    static let vStopStreaming: UInt8 = 3
    static let vBufferUpdate: UInt8 = 5
    static let vRequestKeyframe: UInt8 = 6
    static let vRequestStreaming: UInt8 = 7

    static let vcapCodecs: UInt8 = 1, vcapDisplay: UInt8 = 2, vcapMaxSize: UInt8 = 3, vcapMaxFps: UInt8 = 4
    static let vcapBufferSize: UInt8 = 5, vcapSuggested: UInt8 = 6, vcapL2capPsm: UInt8 = 7, vcapPlayout: UInt8 = 8
    static let vcapTiles: UInt8 = 9, vcapTileSplit: UInt8 = 10, vcapUi: UInt8 = 11, vcapUiStill: UInt8 = 12
    static let vpContent: UInt8 = 8, contentUi: UInt8 = 1
    /* UI input (mg_command_ui_input): sub-commands and features. */
    static let cmdUiInput: UInt8 = 0x86, cmdSubFeature: UInt8 = 0xFE
    static let uiTouchEnable: UInt8 = 0, uiTouch: UInt8 = 1, uiHidMap: UInt8 = 2, uiHidStatus: UInt8 = 3, uiControl: UInt8 = 4
    /* mg_ui_control_t */
    static let ctlFisheye: UInt8 = 1, ctlPan: UInt8 = 2, ctlQuality: UInt8 = 3, ctlStop: UInt8 = 4, ctlMenu: UInt8 = 5, ctlEdgePan: UInt8 = 6
    static let ctlOverscroll: UInt8 = 7   /* 0 none, 1 vertical, 2 all (mg_ui_control_overscroll) */
    static let stillRgb565Zlib: UInt8 = 1
    static let uiHidKey: UInt8 = 5
    static let touchIdPan: UInt8 = 0x80        /* a touch event's id: the gesture pans the mirror (MG_TOUCH_ID_PAN) */
    static let hidMapFisheye: UInt8 = 1, hidMapOffset: UInt8 = 2

    static let vpCodec: UInt8 = 1, vpWidth: UInt8 = 2, vpHeight: UInt8 = 3, vpFps: UInt8 = 4, vpBitrate: UInt8 = 5
    static let vpPlayout: UInt8 = 6, vpTiles: UInt8 = 7
    static let bufferDrop: UInt8 = 1

    /* An mg_error_code_t as a word ("busy"; "code N" for one this app doesn't know). */
    static func errorName(_ code: Int) -> String {
        let names = ["", "unsupported", "invalid length", "invalid value", "busy", "not found", "proof required",
                     "insecure", "recovery", "press required"]
        return code > 0 && code < names.count ? names[code] : "code \(code)"
    }

    static func u16(_ b: [UInt8], _ o: Int) -> Int {
        Int(b[o]) | Int(b[o + 1]) << 8
    }

    static func u32(_ b: [UInt8], _ o: Int) -> Int64 {
        Int64(b[o]) | Int64(b[o + 1]) << 8 | Int64(b[o + 2]) << 16 | Int64(b[o + 3]) << 24
    }
}

/*
 * One musegadgets connection over CoreBluetooth, as Android's GadgetLink:
 * scan, connect, subscribe, and stream video frames on Data with Write
 * Without Response inside the device's credits (buffer_update), Control
 * messages first; an SBC or LC3 tone loop beside the video, interleaved on
 * Data with change_data_type; the gadget's mic and log mirror; a raw
 * throughput bench; the stream on an L2CAP channel where the gadget offers
 * one (useL2cap). iOS picks the MTU and the connection interval itself, and
 * gives no addresses: a gadget is known by its peripheral identifier.
 */
final class GadgetLink: VideoLink, CBCentralManagerDelegate, CBPeripheralDelegate, StreamDelegate {
    private weak var l: VideoLinkListener?
    /* A closed link tells nothing more: its listener may drive a newer link by now. */
    private var listener: VideoLinkListener? { closed ? nil : l }
    private let q = DispatchQueue(label: "mg-ble", qos: .userInteractive)
    private var cm: CBCentralManager!
    private var target: String?, targetAddr: String?
    private var per: CBPeripheral?
    private var perName = ""
    private var advNames: [String: String] = [:]
    private var control: CBCharacteristic?, data: CBCharacteristic?, nusTx: CBCharacteristic?
    private var closed = false, isReady = false
    /* Connections in a row that dropped before they were set up (a gadget
     * without the musegadgets service, say): each waits twice as long before
     * the next try, up to 30 s, instead of hammering it every second. */
    private var failedConnects = 0
    private var connectPaused = false
    private let theCaps = Caps()
    private var psm: CBL2CAPPSM = 0
    private var coc: CBL2CAPChannel?
    /* per, control, data and coc change under lock (sendLoop reads them on its own
     * thread); epoch counts connections, so a write taken for one never reaches the next. */
    private var epoch = 0

    /* Outgoing: Control first, then audio due, then frames within credits. */
    private let lock = NSCondition()
    private var controlQ: [Data] = []
    private var controlWriting = false   /* a Control message taken from controlQ, not yet written */
    private var frameQ: [Data] = []
    private var cur: Data?
    private var curOff = 0
    private var queued: Int64 = 0
    private var isStreaming = false
    private var sent: Int64 = 0
    private var creditAvail: Int64 = 0, creditRecv: Int64 = 0
    /* Bench (-vbuf N): treat the gadget's ring as N bytes (frame caps, credits), to try a small gadget's limits on a big one. */
    var bufferOverride: Int64 = 0
    private var maxAhead: Int64 = 16384
    private var bufferSize: Int64 = 65536
    private var lastRecv: Int64 = -1, lastRecvMs: Int64 = 0
    /* The last start_streaming and whether the device answered it (buffer_update). */
    private var startMsg: Data?, startAtMs: Int64 = 0, startResends = 0, gotCredit = false
    var statLost: Int64 = 0
    /* Bytes written but not yet at the device: enough to keep the radio busy across the credit round trip. */
    var airCap: Int64 = 3072
    /* Largest Data write: 495 + 4 (L2CAP) + 3 (ATT) is two full 251-byte link packets. */
    var writeMax = 495
    var useL2cap = false

    /* Audio playback beside the video (stream_audio). */
    private var videoType = Mg.typeVideoH264
    private var audioType = Mg.typeSbc, audioFrameUs = 8000
    private static let audioPrebuffer = 40, audioPerWrite = 8, audioMinPerWrite = 5
    private var audioOnFlag = false
    private var sbc = Data(), sbcFrameBytes = 60, sbcPos = 0
    private var audioStartNs: Int64 = 0, audioFramesSent: Int64 = 0
    private var aAvail: Int64 = 0, aRecv: Int64 = 0, aSent: Int64 = 0
    private var txType = Mg.typeVideoH264
    var statAudioBytes: Int64 = 0, statTypeSwitches: Int64 = 0, statDataIn: Int64 = 0
    /* -mic: the gadget's mic stream as received (written on the BLE queue, closed from main: under micLock). */
    private let micLock = NSLock()
    private var micFile: FileHandle?
    var micOut: FileHandle? {
        get {
            micLock.lock()
            defer { micLock.unlock() }
            return micFile
        }
        set {
            micLock.lock()
            micFile = newValue
            micLock.unlock()
        }
    }
    /* Closes the mic file under micLock, so no write lands on a closed handle. */
    func closeMic() {
        micLock.lock()
        try? micFile?.close()
        micFile = nil
        micLock.unlock()
    }

    func syncMic() {
        micLock.lock()
        try? micFile?.synchronize()
        micLock.unlock()
    }
    var onDeviceLog: ((String) -> Void)?
    /* [ui_input, ...] notifications (touch events, hid_status), on the BLE queue. */
    var onUiInput: (([UInt8]) -> Void)?

    /* The gadget's Device Information model number ("M5Stack StopWatch"), "" if none. */
    private(set) var model = ""

    /* Token proof: with a key, every connection proves the gadget holds it (TokenProof). */
    var proofKey: [UInt8]?
    /* Else the key for the connected gadget's name, looked up at each connection. */
    var proofKeyFor: ((String) -> [UInt8]?)?
    /* The proof's outcome, on the BLE queue; nil: the gadget offers no token proof. */
    var onProof: ((TokenProof.Outcome?) -> Void)?
    /* A refused command (mg_command_error: the command, the code), on the BLE queue. */
    var onCommandError: ((Int, Int) -> Void)?
    /* Match only this exact name (else any name containing the target). */
    var exactName = false
    private var proof: TokenProof?
    private var provenFlag = false, proofFinished = false   /* under lock */
    /* This stream asked for the gadget's touches (setTouchEvents): under lock, reset by each stream's start
     * and stop and by a disconnect, so a later connection's touches go nowhere until its own stream asks. */
    private var touchOn = false
    private var mustProve = true   /* under lock */
    private var connection = 0   /* counts connections, for the proof's command-list wait */

    init(listener: VideoLinkListener) {
        l = listener
        super.init()
    }

    override func ready() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return isReady
    }

    /* Streams and their frames go only to a gadget that proved its key on this
     * connection (the broadcast); the app's own link asks it of its screen mirror only. */
    var proofRequired: Bool {
        get {
            lock.lock()
            defer { lock.unlock() }
            return mustProve
        }
        set {
            lock.lock()
            mustProve = newValue
            lock.unlock()
        }
    }

    var hasProofKey: Bool {
        lock.lock()
        defer { lock.unlock() }
        return proofKey != nil
    }

    /* This connection's token proof matched. */
    override var proven: Bool {
        lock.lock()
        defer { lock.unlock() }
        return provenFlag
    }

    private var proofIsFinished: Bool {
        lock.lock()
        defer { lock.unlock() }
        return proofFinished
    }

    /* A proof is running, or due once the gadget lists its commands. */
    var proofPending: Bool {
        lock.lock()
        defer { lock.unlock() }
        return proofKey != nil && isReady && !proofFinished
    }

    private func nameMatches(_ name: String) -> Bool {
        guard let t = target else { return false }
        return exactName ? name == t : name.contains(t)
    }

    private func startProof(_ k: [UInt8]) {
        let tp = TokenProof(key: k, send: { [weak self] m in self?.sendControl(Data(m)) })
        proof = tp
        mlog("token proof: key \(TokenProof.fingerprint(k))")
        tp.start(nowMs: nowMs())
        tickProof()
    }

    private func tickProof() {
        guard let tp = proof, tp.pending else { return }
        tp.tick(nowMs: nowMs())
        if let o = tp.outcome {
            proofDone(o)
        } else {
            q.asyncAfter(deadline: .now() + .milliseconds(100)) { [weak self] in self?.tickProof() }
        }
    }

    /* Once per connection: the first outcome stands. */
    private func proofDone(_ o: TokenProof.Outcome) {
        lock.lock()
        if proofFinished {
            lock.unlock()
            return
        }
        provenFlag = o == .match
        proofFinished = true
        lock.unlock()
        proof = nil
        mlog("token proof: \(o)")
        onProof?(o)
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
        let p = per, l2 = coc != nil
        lock.unlock()
        guard let p else { return "BLE, not connected" }
        return "BLE  write \(p.maximumWriteValueLength(for: .withoutResponse)) B" + (l2 ? "  L2CAP" : "")
    }

    override func start() {
        closed = false
        cm = CBCentralManager(delegate: self, queue: q)
        let t = Thread { [weak self] in self?.sendLoop() }
        t.name = "mg-send"
        t.qualityOfService = .userInteractive
        t.start()
    }

    override func close() {
        closed = true
        q.sync {
            cm?.stopScan()
            if let p = per {
                cm?.cancelPeripheralConnection(p)
            }
        }
        lock.lock()
        isReady = false
        touchOn = false
        epoch += 1
        lock.broadcast()
        lock.unlock()
    }

    /* On q, for a new connection or a new key: the key (proofKeyFor), and the
     * proof starts once the gadget lists its commands. */
    private func armProof() {
        /* With exactName the key is the target's: a peripheral picks the name it reports. */
        let k = proofKeyFor.map { f in exactName ? target.flatMap(f) : (f(perName) ?? target.flatMap(f)) } ?? proofKey
        proof = nil
        lock.lock()
        proofKey = k
        provenFlag = false
        proofFinished = false
        lock.unlock()
        guard k != nil else { return }
        /* A gadget that never lists its commands never proves anything: a timeout. */
        connection += 1
        let n = connection
        q.asyncAfter(deadline: .now() + .milliseconds(Int(TokenProof.silenceMs))) { [weak self] in
            guard let self, n == self.connection, self.proof == nil, self.proofPending else { return }
            self.proofDone(.timeout)
        }
    }

    /* The connected gadget's key changed (proofKeyFor): prove it again, from the start. */
    func reprove() {
        lock.lock()
        provenFlag = false   /* now: an outcome or onReady already queued sees no match */
        lock.unlock()
        q.async { [self] in
            guard ready() else { return }
            lock.lock()
            provenFlag = false   /* with the queue and the epoch, before the new key is looked up */
            proofFinished = false
            if mustProve {   /* frames queued on the old proof stay here */
                frameQ.removeAll()
                cur = nil
                queued = 0
            }
            epoch += 1
            lock.unlock()
            armProof()
            sendControl(Data([Mg.cmdRequestStatus]))   /* its command list starts the proof */
        }
    }

    /* ---- scan and connect ---------------------------------------------- */

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        if c.state == .poweredOn {
            scan()
        } else {
            listener?.onStatus(c.state == .unauthorized ? "Needs Bluetooth permission" : "Bluetooth is off")
        }
    }

    private func scan() {
        guard !closed, let cm, cm.state == .poweredOn else { return }   /* nil: a link not started yet */
        /* Already connected to the target from another app (the app, for its
         * broadcast extension): no advertisement to wait for. */
        if per == nil && !connectPaused && (target != nil || targetAddr != nil) {
            for p in cm.retrieveConnectedPeripherals(withServices: [Mg.service]) {
                let name = p.name ?? ""
                let match = targetAddr != nil ? targetAddr!.caseInsensitiveCompare(p.identifier.uuidString) == .orderedSame
                    : nameMatches(name)
                if match {
                    mlog("\(name) is connected already; using that link")
                    lock.lock()
                    per = p
                    lock.unlock()
                    perName = target ?? name
                    p.delegate = self
                    listener?.onStatus("Connecting to \(perName)...")
                    cm.connect(p)
                    return
                }
            }
        }
        if !connectPaused {
            listener?.onStatus(target == nil && targetAddr == nil ? "Pick a gadget (Gadget tab)" : "Looking for \(target ?? targetAddr!)...")
        }
        cm.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData ad: [String: Any], rssi: NSNumber) {
        /* The advertised name: CBPeripheral.name is iOS's cached GAP name, which can be one the
         * gadget had before a firmware update (listed as MuseGadget-1A2B3C when it now says MuseGadget-XN54-1A2B3C).
         * An advertisement without a name (no scan response yet) uses the last one it sent. */
        let addr = p.identifier.uuidString
        if let n = ad[CBAdvertisementDataLocalNameKey] as? String, !n.isEmpty {
            if advNames.count > 512 {
                advNames.removeAll()   /* a long scan in a busy place: start over */
            }
            advNames[addr] = n
        }
        guard let name = advNames[addr] ?? p.name else { return }
        let uuids = ad[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []
        let gadget = uuids.contains(Mg.service) || name.hasPrefix("MuseGadget") || name.hasPrefix("Muse")
        if gadget {
            listener?.onSeen(name: name, address: addr, rssi: rssi.intValue)
        }
        let match = targetAddr != nil ? targetAddr!.caseInsensitiveCompare(addr) == .orderedSame : nameMatches(name)
        if !match || per != nil || connectPaused {
            return
        }
        mlog("found \(name) \(addr) rssi \(rssi)")
        c.stopScan()
        lock.lock()
        per = p
        lock.unlock()
        perName = name
        p.delegate = self
        listener?.onStatus("Connecting to \(name)...")
        c.connect(p)
    }

    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        mlog("connected; max write \(p.maximumWriteValueLength(for: .withoutResponse)) B")
        p.discoverServices([Mg.service, Mg.nusService, Mg.dis])
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        mlog("connect failed: \(error?.localizedDescription ?? "?")")
        disconnected()
    }

    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        mlog("disconnected" + (error != nil ? ": \(error!.localizedDescription)" : ""))
        disconnected()
    }

    func disconnected() {
        nusTx = nil
        proof = nil
        lock.lock()
        per = nil
        control = nil
        data = nil
        coc = nil
        epoch += 1
        let wasReady = isReady
        isReady = false
        provenFlag = false
        proofFinished = false
        isStreaming = false
        touchOn = false
        frameQ.removeAll()
        controlQ.removeAll()
        cur = nil
        queued = 0
        lock.broadcast()
        lock.unlock()
        listener?.onDisconnected(wasReady: wasReady)
        if !closed {
            failedConnects = wasReady ? 0 : failedConnects + 1
            let wait = min(30, 1 << min(5, failedConnects))
            if failedConnects > 1 {
                listener?.onStatus("Couldn't set up the gadget; trying again in \(wait) s")
            }
            q.asyncAfter(deadline: .now() + .seconds(wait)) { [weak self] in
                guard let self, !self.closed, self.per == nil else { return }
                self.scan()
            }
        }
    }

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        let s = p.services?.first { $0.uuid == Mg.service }
        guard let svc = s else {
            listener?.onStatus("No musegadgets service")
            cm.cancelPeripheralConnection(p)
            return
        }
        mlog("musegadgets service")
        model = ""
        if let d = p.services?.first(where: { $0.uuid == Mg.dis }) {
            p.discoverCharacteristics([Mg.disModel], for: d)   /* logged: which board it is */
        }
        p.discoverCharacteristics(nil, for: svc)
        if let nus = p.services?.first(where: { $0.uuid == Mg.nusService }) {
            p.discoverCharacteristics([Mg.nusTx], for: nus)
        }
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        if s.uuid == Mg.dis {
            if let m = s.characteristics?.first(where: { $0.uuid == Mg.disModel }) {
                p.readValue(for: m)
            }
            return
        }
        if s.uuid == Mg.nusService {
            nusTx = s.characteristics?.first { $0.uuid == Mg.nusTx }
            if let n = nusTx {
                p.setNotifyValue(true, for: n)   /* the device's log mirror: its decode stats */
            }
            return
        }
        let ctl = s.characteristics?.first { $0.uuid == Mg.control }, dat = s.characteristics?.first { $0.uuid == Mg.data }
        lock.lock()
        control = ctl
        data = dat
        lock.unlock()
        guard let c = ctl, let d = dat else {
            listener?.onStatus("No Control/Data characteristics")
            cm.cancelPeripheralConnection(p)
            return
        }
        p.setNotifyValue(true, for: c)
        p.setNotifyValue(true, for: d)
    }

    func peripheral(_ p: CBPeripheral, didUpdateNotificationStateFor c: CBCharacteristic, error: Error?) {
        if let e = error {
            mlog("subscribe \(c.uuid): \(e.localizedDescription)")
            return
        }
        guard c.uuid == control?.uuid, c.isNotifying else { return }
        lock.lock()
        isReady = true
        failedConnects = 0
        theCaps.set(Caps())   /* this connection's gadget reports its own: nothing kept from another */
        lock.broadcast()
        lock.unlock()
        armProof()
        sendControl(Data([Mg.cmdRequestStatus]))
        sendControl(Data([Mg.cmdStreamVideo, Mg.vRequestCapabilities]))
        listener?.onStatus("Connected to \(perName) over BLE")
    }

    /* The gadget's GAP name read after connecting (it replaces iOS's cached one). */
    func peripheralDidUpdateName(_ p: CBPeripheral) {
        guard let n = p.name, !n.isEmpty, n != perName else { return }
        if exactName {
            mlog("name: the gadget reports \(n); kept as \(perName) (exact name)")   /* it names its key */
            return
        }
        mlog("name: \(n) (was \(perName))")
        perName = n
        advNames[p.identifier.uuidString] = n
        listener?.onStatus("Connected to \(n) over BLE")
    }

    func peripheralIsReady(toSendWriteWithoutResponse p: CBPeripheral) {
        lock.lock()
        lock.broadcast()
        lock.unlock()
    }

    override func scanForPicker() {
        q.async { [self] in
            connectPaused = true
            cm?.stopScan()
            scan()
        }
    }

    override func pickerClosed() {
        q.async { [self] in
            connectPaused = false
            if per != nil {
                cm?.stopScan()
            }
        }
    }

    override func select(address: String, name: String) {
        q.async { [self] in
            connectPaused = false
            targetAddr = address
            target = name
            cm?.stopScan()
            if let p = per {
                if p.identifier.uuidString.caseInsensitiveCompare(address) == .orderedSame {
                    return   /* already this one */
                }
                cm.cancelPeripheralConnection(p)   /* the disconnect rescans and finds the new target */
            } else {
                scan()
            }
        }
    }

    override func connectedAddress() -> String? {
        ready() ? per?.identifier.uuidString : nil
    }

    override func kind() -> String { "BLE" }

    override func gadgetName() -> String? {
        ready() ? (perName.isEmpty ? (target ?? per?.name) : perName) : nil
    }

    /* The gadget's speaker_volume setting (get_settings / set_settings), -1 until it answers. */
    static let cmdGetSettings: UInt8 = 63, cmdSetSettings: UInt8 = 65, settingSpeakerVolume: UInt8 = 10
    private(set) var speakerVolume = -1

    func askSpeakerVolume() {
        sendControl(Data([Self.cmdGetSettings, Self.settingSpeakerVolume]))
    }

    /* Shown once the gadget answers (a gadget that refuses it keeps its own). */
    func setSpeakerVolume(_ v: Int) {
        sendControl(Data([Self.cmdSetSettings, Self.settingSpeakerVolume, UInt8(max(0, min(100, v)))]))
        askSpeakerVolume()
    }

    /* ---- incoming ------------------------------------------------------ */

    func peripheral(_ p: CBPeripheral, didUpdateValueFor c: CBCharacteristic, error: Error?) {
        guard let v = c.value else { return }
        if c.uuid == Mg.disModel {
            model = String(decoding: v, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines.union(.controlCharacters))
            mlog("model: \(model)")
        } else if c.uuid == Mg.data {
            statDataIn += Int64(v.count)   /* mic audio (start_mic) */
            micLock.lock()
            try? micFile?.write(contentsOf: v)
            micLock.unlock()
        } else if c.uuid == Mg.control {
            onControl([UInt8](v))
        } else if c.uuid == Mg.nusTx {
            for line in String(decoding: v, as: UTF8.self).split(separator: "\n") where !line.isEmpty {
                onDeviceLog?(String(line))
            }
        }
    }

    func onControl(_ v: [UInt8]) {
        guard let cmd = v.first else { return }
        if let tp = proof, tp.onControl(v, nowMs: nowMs()) {
            if let o = tp.outcome {
                proofDone(o)
            }
            return
        }
        if cmd == TokenProof.cmd {
            return   /* a proof message with no proof running (a late or extra one) */
        }
        if cmd == Self.cmdGetSettings {
            /* (setting, value) pairs; only speaker_volume (one byte) is asked for */
            var i = 1
            while i + 1 < v.count && v[i] == Self.settingSpeakerVolume {
                speakerVolume = Int(v[i + 1])
                i += 2
            }
            return
        }
        if cmd == Mg.cmdSupportedFeatures && v.count > 2 && v[1] == Mg.cmdSubFeature && v[2] == Mg.cmdUiInput {
            let f = v.dropFirst(3)
            theCaps.uiTouch = f.contains(1)
            theCaps.hidDigitizer = f.contains(2)
            theCaps.hidMouseAbs = f.contains(3)
            theCaps.hidMouseRel = f.contains(4)
            theCaps.uiControls = f.contains(5)
            mlog("ui input: touch \(theCaps.uiTouch), HID digitizer \(theCaps.hidDigitizer) abs \(theCaps.hidMouseAbs) rel \(theCaps.hidMouseRel), controls \(theCaps.uiControls)")
            return
        }
        if cmd == Mg.cmdSupportedFeatures && v.count > 2 && v[1] == Mg.cmdSubFeature && v[2] == Mg.cmdRequestStatus {
            mlog("the gadget is in a recovery mode")
            proofDone(.recovery)   /* trust nothing from it, with or without a key, and run no proof */
            lock.lock()
            provenFlag = false
            isStreaming = false
            frameQ.removeAll()
            lock.unlock()
            return
        }
        if cmd == Mg.cmdUiInput {
            /* Touches only while this connection's stream asked for them, controls only during a stream:
             * a gadget answering to the saved name after the stream reaches nothing (hid_status passes). */
            if v.count >= 2 && (v[1] == Mg.uiTouch || v[1] == Mg.uiControl) {
                lock.lock()
                let on = isStreaming && (v[1] == Mg.uiControl || touchOn)
                lock.unlock()
                guard on else { return }
            }
            onUiInput?(v)
            return
        }
        if cmd == Mg.cmdSupportedFeatures && v.count > 1 && v[1] != 0xFE {
            let f = v.dropFirst()
            if f.contains(Mg.cmdStreamVideo) {
                theCaps.video = true
            }
            if let k = proofKey, proof == nil, !proofIsFinished {
                if f.contains(TokenProof.cmd) {
                    startProof(k)
                } else {
                    mlog("token proof: the gadget doesn't offer it")
                    lock.lock()
                    proofFinished = true
                    lock.unlock()
                    onProof?(nil)
                }
            }
            return
        }
        if cmd == Mg.cmdError {
            let forCmd = v.count > 1 ? Int(v[1]) : -1, code = TokenProof.errorCode(v)
            mlog("device error for command \(forCmd) code \(code)")
            if forCmd == Int(Self.cmdSetSettings) {
                askSpeakerVolume()   /* show what the gadget kept */
            }
            onCommandError?(forCmd, code)
            if forCmd == Int(Mg.cmdStreamAudio) {
                lock.lock()
                audioOnFlag = false
                txType = videoType
                lock.unlock()
            }
            if forCmd == Int(Mg.cmdStreamVideo) && streaming() {
                lock.lock()
                isStreaming = false
                frameQ.removeAll()
                cur = nil
                queued = 0
                lock.unlock()
                listener?.onStartRefused(Mg.errorName(code))
            }
            return
        }
        if cmd == 30 && v.count >= 19 && v[1] == 11 {
            let bytes = Mg.u32(v, 3), packets = Mg.u32(v, 7), ms = Mg.u32(v, 11), gaps = Mg.u32(v, 15)
            mlog("bench: gadget got \(bytes) bytes, \(packets) packets in \(ms) ms = \(bytes * 8 / max(1, ms)) kb/s, gaps \(gaps)")
        }
        if cmd == Mg.cmdStreamAudio && v.count >= 2 {
            if v[1] == 5 && v.count >= 10 {
                lock.lock()
                aAvail = Mg.u32(v, 2)
                aRecv = Mg.u32(v, 6)
                lock.broadcast()
                lock.unlock()
            } else if v[1] == 3 {
                let kept = v.count >= 3 && v[2] == 0
                mlog("audio: the gadget stopped playback (" + (kept ? "played out" : "dropped") + ")")
                lock.lock()
                audioOnFlag = false
                txType = videoType
                lock.unlock()
            }
            return
        }
        guard cmd == Mg.cmdStreamVideo, v.count >= 2 else { return }
        switch v[1] {
        case Mg.vReportCapabilities:
            guard let p = Self.applyCapsReport(v, to: theCaps, bufferOverride: bufferOverride) else {
                mlog("capabilities: malformed report (\(v.count) bytes), ignored")
                listener?.onStatus("The gadget sent a malformed capability report")
                return
            }
            if p != Self.noPsm {
                psm = CBL2CAPPSM(p)
            }
            if psm != 0 && useL2cap && coc == nil, let p = per {
                mlog("opening an L2CAP channel on PSM \(psm)")
                p.openL2CAPChannel(psm)
            }
            lock.lock()
            bufferSize = theCaps.buffer
            lock.unlock()
            mlog("capabilities: \(theCaps)")
            listener?.onReady(theCaps)
        case Mg.vBufferUpdate where v.count >= 10:
            let avail = Mg.u32(v, 2), recv = Mg.u32(v, 6)
            lock.lock()
            gotCredit = true
            creditAvail = avail
            creditRecv = recv
            /* Self-heal: the ring is empty and the count hasn't moved for a while,
             * yet bytes seem to be on their way: they were lost. */
            let now = nowMs()
            if recv != lastRecv {
                lastRecv = recv
                lastRecvMs = now
            } else if avail == bufferSize && sent > recv && now - lastRecvMs > 400 {
                mlog("credits: \(sent - recv) bytes sent never arrived; resyncing")
                statLost += sent - recv
                sent = recv
                lastRecvMs = now
            }
            lock.broadcast()
            lock.unlock()
            listener?.onBufferUpdate(avail: avail, received: recv, shown: v.count >= 12 ? Mg.u16(v, 10) : -1,
                              dropped: v.count >= 14 ? Mg.u16(v, 12) : -1, decodeMs: v.count >= 15 ? Int(v[14]) : -1)
            if v.count >= 19 {
                meter?.shown(Mg.u16(v, 15), ageMs: Mg.u16(v, 17), rxNs: nowNs())
            }
        case Mg.vRequestKeyframe:
            listener?.onKeyframeRequest()
        case Mg.vRequestStreaming:
            listener?.onRequestStreaming(v.count >= 3 && v[2] != 0)
        case Mg.vStopStreaming:
            lock.lock()
            isStreaming = false
            frameQ.removeAll()
            cur = nil
            queued = 0
            lock.unlock()
            mlog("gadget stopped the stream")
            listener?.onStreamStopped()
        default:
            break
        }
    }

    /* Parses a report into a copy of caps, clamps it and only then publishes
     * it: the PSM or noPsm, or nil with caps untouched. */
    static func applyCapsReport(_ v: [UInt8], to caps: Caps, bufferOverride: Int64) -> Int? {
        let c = caps.copy()
        guard let p = parseCaps(v, into: c) else { return nil }
        if bufferOverride > 0 {
            c.buffer = min(c.buffer, bufferOverride)   /* bench: act as a smaller gadget (-vbuf) */
        }
        c.clamp()
        c.video = true
        caps.set(c)
        return p
    }

    static let noPsm = -1

    /* Reads a report_capabilities message into c: the L2CAP PSM it carries,
     * noPsm when none, nil when the count is missing, a field runs past the
     * end or fewer entries arrive than counted. An unknown id up to 16 ends
     * the report there; one from 17 on is skipped by its length. Fields the
     * report doesn't carry keep c's values, except the codecs, ui and
     * ui_still ones, which it clears. */
    static func parseCaps(_ v: [UInt8], into c: Caps) -> Int? {
        guard v.count > 2 else { return nil }
        let n = Int(v[2])
        var i = 3, psm = noPsm
        c.vp8 = false
        c.uiW = 0
        c.uiH = 0
        c.uiStill = 0
        var e = 0
        while e < n && i < v.count {
            let cap = v[i]
            i += 1
            let width: Int
            switch cap {
            case Mg.vcapCodecs: width = i < v.count ? 1 + Int(v[i]) : 1
            case Mg.vcapDisplay: width = 5
            case Mg.vcapMaxSize, Mg.vcapBufferSize: width = 4
            case Mg.vcapMaxFps, Mg.vcapTiles, Mg.vcapTileSplit, Mg.vcapUiStill: width = 1
            case Mg.vcapL2capPsm, Mg.vcapPlayout: width = 2
            case Mg.vcapUi: width = 10
            case Mg.vcapSuggested: width = 7
            default:
                guard cap >= 17 else { return psm }   /* no length up to 16: the rest can't be found */
                width = i < v.count ? 1 + Int(v[i]) : 1   /* from 17 on: a length, skipped */
            }
            guard i + width <= v.count else { return nil }
            switch cap {
            case Mg.vcapCodecs:
                if v[(i + 1)..<(i + width)].contains(Mg.typeVideoVp8) {
                    c.vp8 = true
                }
            case Mg.vcapDisplay:
                c.dispW = Mg.u16(v, i)
                c.dispH = Mg.u16(v, i + 2)
                c.round = v[i + 4] != 0
            case Mg.vcapMaxSize:
                c.maxW = Mg.u16(v, i)
                c.maxH = Mg.u16(v, i + 2)
            case Mg.vcapMaxFps:
                c.maxFps = Int(v[i])
            case Mg.vcapBufferSize:
                c.buffer = Mg.u32(v, i)
            case Mg.vcapL2capPsm:
                psm = Mg.u16(v, i)
            case Mg.vcapPlayout:
                c.maxPlayoutMs = Mg.u16(v, i)
            case Mg.vcapTiles:
                c.maxTiles = max(1, Int(v[i]))
            case Mg.vcapTileSplit:
                c.tileSplitPct = Int(v[i])
            case Mg.vcapUi:
                c.uiW = Mg.u16(v, i)
                c.uiH = Mg.u16(v, i + 2)
                c.uiFps = Int(v[i + 4])
                c.uiMaxFrame = Int(Mg.u32(v, i + 5))
                c.uiBands = max(1, Int(v[i + 9]))
            case Mg.vcapUiStill:
                c.uiStill = Int(v[i])
            case Mg.vcapSuggested:
                c.sugW = Mg.u16(v, i)
                c.sugH = Mg.u16(v, i + 2)
                c.sugFps = Int(v[i + 4])
                c.sugKbps = Mg.u16(v, i + 5)
            default:
                break
            }
            i += width
            e += 1
        }
        return e < n ? nil : psm
    }

    func peripheral(_ p: CBPeripheral, didOpen ch: CBL2CAPChannel?, error: Error?) {
        guard let ch, error == nil else {
            mlog("L2CAP channel failed; staying on GATT: \(error?.localizedDescription ?? "?")")
            return
        }
        ch.outputStream?.schedule(in: .main, forMode: .default)
        ch.outputStream?.delegate = self
        ch.outputStream?.open()
        ch.inputStream?.open()
        mlog("L2CAP channel open on PSM \(ch.psm)")
        lock.lock()
        coc = ch
        lock.broadcast()
        lock.unlock()
    }

    func stream(_ s: Stream, handle e: Stream.Event) {
        if e == .hasSpaceAvailable {
            lock.lock()
            lock.broadcast()
            lock.unlock()
        } else if e == .errorOccurred || e == .endEncountered {
            mlog("L2CAP channel closed; back to GATT")
            lock.lock()
            coc = nil
            lock.unlock()
        }
    }

    /* ---- outgoing ------------------------------------------------------ */

    /* The Control messages waiting to go, taken (host tests). */
    func takeControl() -> [Data] {
        lock.lock()
        defer { lock.unlock() }
        let m = controlQ
        controlQ.removeAll()
        return m
    }

    /* Waits (at most maxMs) until the Control messages queued so far are written, before a close. */
    func flushControl(maxMs: Int) {
        let until = Date().addingTimeInterval(Double(maxMs) / 1000)
        lock.lock()
        while (!controlQ.isEmpty || controlWriting) && isReady && !closed && Date() < until {
            _ = lock.wait(until: until)
        }
        lock.unlock()
    }

    func sendControl(_ m: Data) {
        lock.lock()
        controlQ.append(m)
        lock.broadcast()
        lock.unlock()
    }

    /* Plays SBC (16 kHz mono, frameBytes a frame) or LC3 (10 ms frames) on the gadget, looped, beside any video. */
    func startAudio(_ frames: Data, frameBytes: Int, lc3: Bool) {
        lock.lock()
        audioType = lc3 ? Mg.typeLc3 : Mg.typeSbc
        audioFrameUs = lc3 ? 10000 : 8000
        sbc = frames
        sbcFrameBytes = frameBytes
        sbcPos = 0
        audioFramesSent = 0
        aAvail = 0
        aRecv = 0
        aSent = 0
        audioStartNs = 0
        audioOnFlag = true
        txType = isStreaming ? videoType : audioType
        /* SBC: no frame_size (each SBC header carries its length; some gadgets refuse one). */
        controlQ.append(lc3 ? Data([Mg.cmdStreamAudio, 2, 3, 1, audioType, 2, 0x80, 0x3e, 0, 0, 5,
                                    UInt8(frameBytes & 0xff), UInt8(frameBytes >> 8)])
                            : Data([Mg.cmdStreamAudio, 2, 2, 1, audioType, 2, 0x80, 0x3e, 0, 0]))
        lock.broadcast()
        lock.unlock()
        mlog("audio: start_streaming " + (lc3 ? "LC3" : "SBC") + " 16 kHz (\(frameBytes) B frames)")
    }

    func stopAudio() {
        lock.lock()
        defer { lock.unlock() }
        if !audioOnFlag {
            return
        }
        audioOnFlag = false
        txType = videoType
        controlQ.append(Data([Mg.cmdStreamAudio, 3, Mg.bufferDrop]))
        lock.broadcast()
    }

    func audioOn() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return audioOnFlag
    }

    /* Audio frames per Data write: whole frames only (a gadget refuses a write that splits
     * one), as many as fit one write up to audioPerWrite: on an MTU of 247, 6 LC3 frames of
     * 40 B or 4 SBC frames of 60 B. */
    private func perWrite() -> Int {
        let room = coc != nil ? 2048 : min(writeMax, per?.maximumWriteValueLength(for: .withoutResponse) ?? 20)
        return max(1, min(Self.audioPerWrite, room / max(1, sbcFrameBytes)))
    }

    private func minPerWrite() -> Int {
        min(Self.audioMinPerWrite, perWrite())
    }

    /* Under lock: audio frames due and credited for the next write (0: none yet). */
    private func audioFramesDue() -> Int {
        if !audioOnFlag || sbc.isEmpty || aAvail == 0 {
            return 0
        }
        let now = nowNs()
        if audioStartNs == 0 {
            audioStartNs = now
        }
        let due = (now - audioStartNs) / 1000 / Int64(audioFrameUs) + Int64(Self.audioPrebuffer) - audioFramesSent
        let credit = (aAvail - (aSent - aRecv)) / Int64(sbcFrameBytes)
        let k = Int(min(min(due, credit), Int64(perWrite())))
        return k < minPerWrite() ? 0 : k
    }

    private func takeAudio(_ k: Int) -> Data {
        var out = Data(capacity: k * sbcFrameBytes)
        let frames = sbc.count / sbcFrameBytes
        for _ in 0..<k {
            let s = sbc.startIndex + sbcPos * sbcFrameBytes
            out.append(sbc[s..<(s + sbcFrameBytes)])
            sbcPos = (sbcPos + 1) % frames
        }
        return out
    }

    /* Bench: device_action throughput_test receive for secs; writes as fast as iOS takes them. */
    func benchReceive(_ secs: Int) {
        Thread.detachNewThread { [self] in
            sendControl(Data([30, 11, 2, UInt8(secs & 0xff), UInt8(secs >> 8)]))
            sendControl(Data([Mg.cmdChangeDataType, 20]))
            Thread.sleep(forTimeInterval: 1.5)
            lock.lock()
            let ep = epoch, per0 = per, data0 = data
            lock.unlock()
            guard let p = per0, let d = data0 else { return }
            let n = min(writeMax, p.maximumWriteValueLength(for: .withoutResponse))
            let end = nowMs() + Int64(secs) * 1000, t0 = nowMs()
            var bytes = 0, seq = 0
            while nowMs() < end {
                var b = Data(count: n)
                for i in 0..<n {
                    b[i] = i < 4 ? UInt8((seq >> (8 * i)) & 0xff) : UInt8((seq + i - 4) & 0xff)
                }
                if !writeData(p, d, b, epoch: ep) {
                    break
                }
                bytes += n
                seq += 1
            }
            let ms = nowMs() - t0
            mlog("bench: phone wrote \(bytes) bytes in \(ms) ms = \(Int64(bytes) * 8 / max(1, ms)) kb/s")
            sendControl(Data([30, 11, 0]))
        }
    }

    /* bands: horizontal bands, each its own stream (VP_TILES); vp8: VP8 (data type 25), else H.264. */
    override func startStreaming(w: Int, h: Int, fps: Int, kbps: Int, playoutMs: Int, bands: Int = 1, vp8: Bool = false) {
        let codec = vp8 ? Mg.typeVideoVp8 : Mg.typeVideoH264
        let playout = min(playoutMs, theCaps.maxPlayoutMs)
        mlog("start_streaming \(vp8 ? "VP8" : "H.264") \(w)x\(h)@\(fps) \(kbps) kb/s, playout \(playout) ms (was streaming: \(streaming()))")
        var m: [UInt8] = [Mg.cmdStreamVideo, Mg.vStartStreaming, playout > 0 ? 6 : 5,
                          Mg.vpCodec, codec,
                          Mg.vpWidth, UInt8(w & 0xff), UInt8(w >> 8),
                          Mg.vpHeight, UInt8(h & 0xff), UInt8(h >> 8),
                          Mg.vpFps, UInt8(fps),
                          Mg.vpBitrate, UInt8(kbps & 0xff), UInt8(kbps >> 8)]
        if playout > 0 {
            m += [Mg.vpPlayout, UInt8(playout & 0xff), UInt8(playout >> 8)]
        }
        if bands > 1 {
            m[2] += 1
            m += [Mg.vpTiles, UInt8(bands)]
        }
        lock.lock()
        if mustProve && !provenFlag {
            lock.unlock()
            mlog("start_streaming: the gadget hasn't proved its key; not sent")
            return
        }
        videoType = codec
        touchOn = false
        maxAhead = max(12288, Int64(kbps) * 1000 / 8 * Int64(150 + playout) / 1000)
        frameQ.removeAll()
        cur = nil
        queued = 0
        sent = 0
        creditAvail = 0   /* nothing until the device's first buffer_update */
        creditRecv = 0
        isStreaming = true
        txType = audioOnFlag ? audioType : videoType
        controlQ.append(Data(m))
        startMsg = Data(m)
        startAtMs = nowMs()
        startResends = 0
        gotCredit = false
        lock.broadcast()
        lock.unlock()
    }

    override func startUiStreaming(w: Int, h: Int, fps: Int, kbps: Int, bands: Int) {
        mlog("start_streaming UI H.264 \(w)x\(h), \(bands) band(s) (was streaming: \(streaming()))")
        var m: [UInt8] = [Mg.cmdStreamVideo, Mg.vStartStreaming, 6,
                          Mg.vpCodec, Mg.typeVideoH264,
                          Mg.vpWidth, UInt8(w & 0xff), UInt8(w >> 8),
                          Mg.vpHeight, UInt8(h & 0xff), UInt8(h >> 8),
                          Mg.vpFps, UInt8(fps),
                          Mg.vpBitrate, UInt8(kbps & 0xff), UInt8(kbps >> 8),
                          Mg.vpContent, Mg.contentUi]
        if bands > 1 {
            m[2] += 1
            m += [Mg.vpTiles, UInt8(bands)]
        }
        lock.lock()
        if mustProve && !provenFlag {
            lock.unlock()
            mlog("start_streaming: the gadget hasn't proved its key; not sent")
            return
        }
        videoType = Mg.typeVideoH264
        touchOn = false   /* until this stream asks for them (setTouchEvents) */
        maxAhead = 1 << 20   /* frames come rarely; the credits alone pace them */
        frameQ.removeAll()
        cur = nil
        queued = 0
        sent = 0
        creditAvail = 0
        creditRecv = 0
        isStreaming = true
        txType = audioOnFlag ? audioType : videoType
        controlQ.append(Data(m))
        startMsg = Data(m)
        startAtMs = nowMs()
        startResends = 0
        gotCredit = false
        lock.broadcast()
        lock.unlock()
    }

    /* A UI control's value (Mg.ctl*), at the start of a UI stream and on every change. */
    func sendUiControl(_ id: UInt8, _ value: Int) {
        if theCaps.uiControls {
            sendControl(Data([Mg.cmdUiInput, Mg.uiControl, id, UInt8(max(0, min(255, value)))]))
        }
    }

    /* The gadget's HID presses a consumer-control key (0x224 AC Back, 0x223 AC Home). */
    func sendHidKey(_ usage: Int) {
        sendControl(Data([Mg.cmdUiInput, Mg.uiHidKey, UInt8(usage & 0xff), UInt8((usage >> 8) & 0xff)]))
    }

    /*
     * hid_map's square from a crop (x, y, w, h in 0..1 of the screen, which may
     * reach past it by half the window): 0..65535 each, and the offset flag
     * with x, y as (f + 0.5) / 2 when it does (mg_hid_map_offset).
     */
    static func hidSquare(_ c: SIMD4<Float>) -> (x: Int, y: Int, w: Int, h: Int, offset: Bool) {
        func q(_ v: Double) -> Int { Int(max(0, min(65535, (v * 65535).rounded()))) }
        let x = Double(c.x), y = Double(c.y), w = Double(c.z), h = Double(c.w)
        let off = x < 0 || y < 0 || x + w > 1 || y + h > 1
        return (q(off ? (x + 0.5) / 2 : x), q(off ? (y + 0.5) / 2 : y), q(w), q(h), off)
    }

    /* Touch events on (the gadget notifies [ui_input, touch, ...]) or off. */
    func setTouchEvents(_ on: Bool) {
        lock.lock()
        touchOn = on && isStreaming
        lock.unlock()
        sendControl(Data([Mg.cmdUiInput, Mg.uiTouchEnable, on ? 1 : 0]))
    }

    /* hid_map: the streamed square on the phone's screen (0..65535 of its width
     * and height), the warp, and for mouse_rel the pointer counts across it. */
    func sendHidMap(mode: UInt8, x: Int, y: Int, w: Int, h: Int, fisheye: Bool, countsW: Int = 0, countsH: Int = 0,
                    offset: Bool = false) {
        func le(_ v: Int) -> [UInt8] { [UInt8(v & 0xff), UInt8((v >> 8) & 0xff)] }
        var m: [UInt8] = [Mg.cmdUiInput, Mg.uiHidMap, mode]
        if mode != 0 {
            m += le(x) + le(y) + le(w) + le(h) + [(fisheye ? Mg.hidMapFisheye : 0) | (offset ? Mg.hidMapOffset : 0)]
            if mode == 3 {
                m += le(countsW) + le(countsH)
            }
        }
        sendControl(Data(m))
    }

    override func stopStreaming() {
        lock.lock()
        defer { lock.unlock() }
        touchOn = false
        if !isStreaming {
            return
        }
        isStreaming = false
        frameQ.removeAll()
        cur = nil
        queued = 0
        controlQ.append(Data([Mg.cmdStreamVideo, Mg.vStopStreaming, Mg.bufferDrop]))
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
        if !isStreaming || (mustProve && !provenFlag) {
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

    /* Write Without Response when CoreBluetooth has room (else wait for peripheralIsReady);
     * false once the connection it was taken for (epoch) is gone. */
    private func writeData(_ p: CBPeripheral, _ c: CBCharacteristic, _ v: Data, epoch ep: Int) -> Bool {
        let t0 = nowMs()
        while nowMs() - t0 < 5000 {
            var done = false, stale = false
            q.sync {   /* disconnected() and a new connection's setup run on q: the epoch holds meanwhile */
                lock.lock()
                stale = epoch != ep
                lock.unlock()
                if !stale && p.state == .connected && p.canSendWriteWithoutResponse {
                    p.writeValue(v, for: c, type: c.properties.contains(.writeWithoutResponse) ? .withoutResponse : .withResponse)
                    done = true
                }
            }
            if done {
                return true
            }
            if stale || p.state != .connected {
                return false
            }
            lock.lock()
            _ = lock.wait(until: Date().addingTimeInterval(0.005))
            lock.unlock()
        }
        return false
    }

    private func sendLoop() {
        while !closed {
            var ctl: Data?, chunk: Data?
            var audioChunk = false
            var frameDone = -1
            lock.lock()
            while !closed {
                if !isReady {
                    _ = lock.wait(until: Date().addingTimeInterval(0.05))
                    continue
                }
                /* A start the device never answered (iOS drops writes while a pairing
                 * prompt the device asked for is up): send it again, a few times. */
                if isStreaming && !gotCredit && controlQ.isEmpty, let m = startMsg, startResends < 4,
                   nowMs() - startAtMs > 2500 {
                    startResends += 1
                    startAtMs = nowMs()
                    mlog("start_streaming unanswered for 2.5 s; sending it again (\(startResends))")
                    controlQ.append(m)
                }
                if !controlQ.isEmpty {
                    ctl = controlQ.removeFirst()
                    controlWriting = true
                    break
                }
                let k = audioFramesDue()
                if k > 0 {
                    /* The switch goes first; the frames go out on the next pass. */
                    if txType != audioType {
                        txType = audioType
                        statTypeSwitches += 1
                        ctl = Data([Mg.cmdChangeDataType, audioType])
                        break
                    }
                    let a = takeAudio(k)
                    chunk = a
                    audioFramesSent += Int64(a.count / sbcFrameBytes)
                    aSent += Int64(a.count)
                    statAudioBytes += Int64(a.count)
                    audioChunk = true
                    break
                }
                if isStreaming && mustProve && !provenFlag && (cur != nil || !frameQ.isEmpty) {
                    frameQ.removeAll()   /* queued under a proof that no longer holds */
                    cur = nil
                    queued = 0
                }
                if isStreaming && !(mustProve && !provenFlag) {
                    if cur == nil && !frameQ.isEmpty {
                        /* A new frame starts only while the device holds little
                         * (in the air plus unread in its ring). */
                        let backlog = sent - creditRecv + (bufferSize - min(creditAvail, bufferSize))
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
                        let wmax = per?.maximumWriteValueLength(for: .withoutResponse) ?? 20
                        let maxN = coc != nil ? 2048 : min(writeMax, max(20, wmax))
                        let n = min(maxN, f.count - curOff)
                        if sent - creditRecv + Int64(n) <= min(min(creditAvail, bufferSize), airCap) {
                            if txType != videoType && coc == nil {
                                txType = videoType
                                statTypeSwitches += 1
                                ctl = Data([Mg.cmdChangeDataType, videoType])
                                break
                            }
                            let s = f.startIndex + curOff
                            chunk = f.subdata(in: s..<(s + n))
                            curOff += n
                            sent += Int64(n)
                            queued -= Int64(n)
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
            /* What this item goes on, as it stood when it was taken. */
            let ep = epoch, p0 = per, coc0 = coc, control0 = control, data0 = data
            lock.unlock()
            defer {
                if ctl != nil {
                    lock.lock()
                    controlWriting = false
                    lock.broadcast()
                    lock.unlock()
                }
            }
            guard let p = p0 else { continue }
            if let ch = chunk, !audioChunk, let c = coc0, let os = c.outputStream {
                lock.lock()
                let stale = ep != epoch || (mustProve && !provenFlag)
                if stale && ep == epoch {
                    sent -= Int64(ch.count)   /* never went: the credit count doesn't wait for it */
                }
                lock.unlock()
                if stale {
                    continue
                }
                var off = 0
                let t0 = nowMs()
                while off < ch.count && nowMs() - t0 < 2000 {
                    if os.hasSpaceAvailable {
                        let w = ch.withUnsafeBytes { os.write($0.bindMemory(to: UInt8.self).baseAddress! + off, maxLength: ch.count - off) }
                        if w < 0 {
                            break
                        }
                        off += w
                    } else {
                        Thread.sleep(forTimeInterval: 0.002)
                    }
                }
                statBytes += Int64(ch.count)
                statWrites += 1
                continue
            }
            guard let target = ctl != nil ? control0 : data0, let v = ctl ?? chunk else { continue }
            if !writeData(p, target, v, epoch: ep) {
                mlog("write failed")
            } else if chunk != nil {
                statBytes += Int64(v.count)
                statWrites += 1
            }
            if frameDone >= 0 {
                meter?.sent(frameDone, ns: nowNs())
            }
        }
    }
}
