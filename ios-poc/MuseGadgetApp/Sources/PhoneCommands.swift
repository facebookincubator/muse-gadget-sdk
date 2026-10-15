import AVFoundation
import MuseGadgetKit
import UIKit

/// What Muse can do on this iPhone. `commands_v2` uses the same spec shape as
/// the Linux SDK's `COMMAND_SPECS`.
@MainActor
final class PhoneCommands {
    static let specs: [String: Any] = [
        "device.health": [
            "description": "iPhone status: battery level and state, Low Power Mode, thermal state, free storage, iOS version, uptime.",
            "required": [String: Any](), "optional": [String: Any](),
        ],
        "display.show_text": [
            "description": "Show a short message full-screen on the iPhone.",
            "required": ["text": ["type": "string", "description": "Text to show."]],
            "optional": [String: Any](),
        ],
        "speech.say": [
            "description": "Speak text aloud through the iPhone speaker.",
            "required": ["text": ["type": "string", "description": "Text to speak."]],
            "optional": ["language": ["type": "string", "description": "BCP-47 voice language, e.g. en-US."]],
        ],
        "device.flashlight": [
            "description": "Turn the iPhone flashlight on or off.",
            "required": ["on": ["type": "boolean", "description": "true to turn it on."]],
            "optional": [String: Any](),
        ],
    ]

    private let synthesizer = AVSpeechSynthesizer()
    var showText: (String) -> Void = { _ in }

    func run(_ command: String, _ params: [String: Any]) -> [String: Any] {
        switch command {
        case "device.health": return ok(health())
        case "display.show_text":
            guard let text = params["text"] as? String else { return error("text is required") }
            showText(text)
            return ok(["shown": true])
        case "speech.say":
            guard let text = params["text"] as? String else { return error("text is required") }
            let utterance = AVSpeechUtterance(string: text)
            utterance.voice = AVSpeechSynthesisVoice(language: params["language"] as? String)
            synthesizer.speak(utterance)
            return ok(["spoken": true])
        case "device.flashlight":
            guard let on = params["on"] as? Bool else { return error("on is required") }
            guard let torch = AVCaptureDevice.default(for: .video), torch.hasTorch else {
                return error("this device has no flashlight")
            }
            do {
                try torch.lockForConfiguration()
                torch.torchMode = on ? .on : .off
                torch.unlockForConfiguration()
                return ok(["on": on])
            } catch {
                return self.error("flashlight unavailable: \(error.localizedDescription)")
            }
        default:
            return error("unknown command \(command)")
        }
    }

    private func health() -> [String: Any] {
        let device = UIDevice.current
        device.isBatteryMonitoringEnabled = true
        let states: [UIDevice.BatteryState: String] = [.unknown: "unknown", .unplugged: "unplugged",
                                                       .charging: "charging", .full: "full"]
        let thermal = ["nominal", "fair", "serious", "critical"]
        let free = (try? URL(fileURLWithPath: NSHomeDirectory())
            .resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?
            .volumeAvailableCapacityForImportantUsage
        return [
            "model": device.model,
            "system": "\(device.systemName) \(device.systemVersion)",
            "battery_percent": device.batteryLevel < 0 ? NSNull() : Int(device.batteryLevel * 100),
            "battery_state": states[device.batteryState] ?? "unknown",
            "low_power_mode": ProcessInfo.processInfo.isLowPowerModeEnabled,
            "thermal_state": thermal[min(ProcessInfo.processInfo.thermalState.rawValue, 3)],
            "storage_free_bytes": free.map { NSNumber(value: $0) } ?? NSNull(),
            "uptime_s": Int(ProcessInfo.processInfo.systemUptime),
        ]
    }

    private func ok(_ payload: [String: Any]) -> [String: Any] { ["ok": true, "payload": payload] }
    private func error(_ message: String) -> [String: Any] { ["ok": false, "error": message] }
}
