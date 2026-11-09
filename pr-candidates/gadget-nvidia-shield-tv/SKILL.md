---
name: gadget-nvidia-shield-tv
description: >-
  Use an NVIDIA Shield TV as a fixed Wi-Fi sensing point: a sideloaded Android TV app
  samples Wi-Fi scan results, link state and gateway latency over ADB, yielding per-AP
  RSSI time series for coarse presence corroboration. Works while the device is wired
  (scan results need no association). RSSI-class sensing only — Android exposes no
  CSI at any permission level, and the 2019 Pro has no Wi-Fi RTT or Wi-Fi Aware.
---

# NVIDIA Shield TV (Wi-Fi Sense)

Use this skill when a confirmed NVIDIA Shield TV is available as an always-on,
fixed-position Android TV device and the goal is RF sensing or telemetry from it,
not media control (for Cast, use the [Google Cast skill](../gadget-google-cast/SKILL.md)).

## Identify the Device

- Confirm the model (2019 Pro `mdarcy` verified; other generations differ in
  Bluetooth/Wi-Fi hardware) and that ADB is reachable (developer options +
  network debugging, or USB).
- Read the active network honestly: an Ethernet-connected Shield reports
  WifiInfo RSSI −127. That is expected, not a fault — the sensing lane is
  **scan results**, which the radio produces while unassociated.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- ADB access to the device and permission to sideload one app.
- Location permission granted to the app (Android requires it for scan results),
  and a battery-optimization exemption so sampling survives display sleep.
- A host that can poll the device (ADB over LAN/tunnel) on a few-minutes cadence.

## Workflow

1. Install a minimal sampling app (plain Android, no AndroidX needed): it reads
   `WifiManager.getConnectionInfo()` + `getScanResults()`, pings the gateway,
   and appends one JSON line per sample (2 Hz reference rate) to its
   app-specific external files directory. Run sampling in a **foreground
   service** started at boot (`BOOT_COMPLETED` receiver) — an Activity-only
   sampler dies with the UI.
2. Throttle scan requests to the OS cadence (results are cached/throttled;
   effective new-data rate is ~30 s to a few minutes regardless of sample rate).
3. From the host, pull the newest sample over ADB on the poll cadence, extract
   the home APs' RSSI by BSSID, and append one row to an RSSI time series.
   Dedupe on the scan signature; rotate/truncate the on-device log (2 Hz JSONL
   grows ~175 MB/day unrotated).
4. Consume the series as **variance**, not values: per-point RSSI range (or,
   once ≥200 samples across ≥4 days exist, deviation from a learned per-hour
   baseline) is the movement signal. A flat series abstains — fixed,
   always-on-linked points at rest carry ~no occupancy evidence.
5. Keep the vote honest in any presence stack: anonymous, household-level,
   corroborate/challenge only at low weight. It must never fire, block, or
   reverse a person-specific (GPS/phone) decision by itself.

## Verify the Result

- Samples on the host parse as JSON and contain real scan results (dozens of
  APs, home APs strong). A screencap may be black while the display sleeps —
  judge by the sample log, not the screen.
- The service survives the Activity being closed; the sampler relaunches it
  if the process is gone.
- Report per-sample gateway ping and WifiInfo state alongside scan data so an
  Ethernet device is never misread as a Wi-Fi failure.

## Limits

- **No CSI.** Channel State Information is a receiver-PHY firmware export;
  Android has no API for it. This skill yields RSSI-class sensing (coarse
  presence/activity corroboration) — never pose, tracking, people-counting,
  or vitals.
- **No Wi-Fi RTT / Aware on the 2019 Pro** (no `android.hardware.wifi.rtt`,
  no `android.hardware.wifi.aware`) — 802.11mc ranging is unavailable here.
- Scan throttling caps the temporal resolution: occupancy *trends*, not
  motion *transients*. If the device is Wi-Fi-associated, the same app also
  yields a true 2 Hz connected-link RSSI stream.
- Do not change the device's network configuration (e.g. forcing Wi-Fi) to
  "improve" sensing; the Ethernet + scan pattern exists precisely so the
  streaming setup stays untouched.

## Sources

- [Android WifiManager](https://developer.android.com/reference/android/net/wifi/WifiManager)
- [Android Wi-Fi infrastructure concepts (scan throttling, location permission)](https://developer.android.com/develop/connectivity/wifi/wifi-infrastructure)
- Reference implementation: [shield-wifi-sense](https://github.com/jonathancruzate/shield-wifi-sense) (app source, host sampler, honest-scoping notes)
