---
name: gadget-thingino-cameras
description: >-
  Capture still JPEG snapshots or bounded TCP-interleaved RTSP frames from IP cameras already
  running Thingino firmware through Muse Home Link, and optionally format baseline JPEGs for a
  paired Muse Gadget display. Read-only capture; not flashing, camera settings, or PTZ control.
---

# Thingino IP Cameras: Snapshots and Sky/Room Checks

Use this skill when an IP camera on the user's home network already runs Thingino open-source firmware with its local HTTP snapshot or RTSP service enabled.

## Identify the Device

- Confirm with the user or from `device.discover` that the host is an already-installed Thingino camera. Thingino cameras commonly appear via ARP, HTTP (`80/tcp`), RTSP (`554/tcp`), mDNS, or ONVIF WS-Discovery (`3702/udp` when ONVIF is enabled).
- Connect by the camera's IPv4 address from `device.discover` or the user. Multicast `.local` mDNS names do not resolve across the unicast IPv4 Home Link tunnel; suggest a router DHCP reservation so the camera's IP and per-device approval stay stable.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- Thingino is already installed and configured on the camera, with its snapshot token or HTTP/RTSP credentials supplied by the user.
- Per-device Home Link approval for the camera's IPv4 address. On first connection, wait for the user to approve the camera in the Muse app; if the turn came from a mic-less gadget console, let the user know approval is waiting in the Muse app.
- A credential-aware HTTP client or TCP-interleaved RTSP client routed through Home Link. Keep tokens, usernames, and passwords on the VM; never echo them in chat replies or logged command arguments.

## Workflow

1. **Prefer HTTP snapshots first.** Use the endpoint enabled on the user's Thingino build:
   - **Token snapshot endpoints (no web-UI password needed):**
     - `http://<ip>/x/ch1.jpg?token=<token>` — sub-stream JPEG (typically 640x360 or 720p, ~40-150 KB). Prefer this for quick visual checks or when targeting a small gadget screen (`480x480`), as it transfers several times faster over the tunnel.
     - `http://<ip>/x/ch0.jpg?token=<token>` — main-stream JPEG (full sensor resolution). Use when the user asks to read fine detail.
   - **Authenticated web-UI snapshot:** `http://<ip>/image.jpg` (or `/x/image.jpg` on older builds) with HTTP Basic or Digest auth (`curl --anyauth --netrc-file` or a protected curl config file on the VM).
2. **RTSP fallback (TCP only).** If HTTP snapshots are disabled and RTSP is enabled (`rtsp://<ip>:554/ch1` or `/ch0`), grab a single frame with interleaved TCP (`ffmpeg -rtsp_transport tcp -i ... -frames:v 1`). Never use UDP RTP: the Home Link tunnel has a `1280`-byte IPv4 MTU, so TCP clamps MSS cleanly while oversized UDP packets drop.
3. **Optional Muse Gadget display handoff (`display.draw_file`).** When asked to show the view on a paired Muse Gadget screen (such as a `480x480` WeatherXM WG1200), resize/center-crop (and optionally composite any requested caption) on the VM into a **baseline** (non-progressive) JPEG matching the screen dimensions, write it to the VM workspace, and call `display.draw_file`. ESP32 ROM `tjpgd` rejects progressive JPEGs and scales oversized images only by powers of two (`1/2`, `1/4`, `1/8`).
4. Close the connection as soon as the requested frame or bounded capture completes.

## Verify the Result

- Confirm HTTP `200`, `image/jpeg` content (`FF D8` SOI marker), and a decodable, non-cached frame from the intended camera before describing it or sending it to `display.draw_file`.
- Do not mistake an HTML login page, `401`/`403` error body, or open port `80`/`554` for a captured image.

## Limits

- Read-only snapshot and single-frame capture: do not call configuration or control CGIs (`/x/json-*.cgi`, streamer restarts, IR/LED toggles, PTZ, motor calibration, or two-way audio).
- Do not SSH into the camera, modify Thingino settings, run `sysupgrade`, or guide flashing stock cameras to Thingino.

## Sources

- [Thingino Firmware repository and wiki](https://github.com/themactep/thingino-firmware)
- [Thingino project documentation](https://thingino.com/)
- [FFmpeg RTSP TCP interleaved transport](https://ffmpeg.org/ffmpeg-protocols.html#rtsp)
