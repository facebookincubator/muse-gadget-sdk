---
name: gadget-bambu-lab-p2s
description: >-
  Read print status, grab chamber-camera snapshots, monitor a running print, and
  pause a clearly failing print on a Bambu Lab P2S through Muse Home Link,
  using the printer's LAN MQTT and RTSPS interfaces with LAN Only Liveview on
  and cloud mode kept. Not for LAN Only or Developer Mode setups, starting
  prints, or arbitrary control commands.
---

# Bambu Lab P2S

Use this skill when discovery or the user identifies a Bambu Lab P2S on the home network and the user wants print status, a camera snapshot, monitoring while a print runs, or a pause for a print that is clearly failing. Verified end to end on a P2S in cloud mode on 2026-11-05: live status and camera frames both worked through Home Link while Bambu Handy kept working on its cloud path.

## Identify the Device

- Get the printer's IPv4 address and 15-character serial number from the printer touchscreen or Bambu Handy. The P2S does not announce itself by name over mDNS or SSDP, so a network scan alone cannot identify it. Match the address from the touchscreen against the scan results.
- Confirm the model is a P2S before using the camera path. Other Bambu models differ: the P1 and A1 families stream low-rate JPEG frames instead of RTSPS, and those paths are not covered here.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- On the printer touchscreen, set **LAN Only** to off and **LAN Only Liveview** to on. Liveview is a separate toggle from LAN Only Mode: with Liveview on and LAN Only off, the printer stays on Bambu Cloud, Bambu Handy and its live video keep working, and the local camera stream becomes available on the LAN. Verify Handy live video after changing the setting, and revert if it breaks.
- The printer's LAN access code and serial number. Store the access code in a local file with restricted permissions and read it from there. Never put the code in a skill, a chat message, a log, or a command line.
- A client route through the Home Link HTTP CONNECT proxy for custom TCP: an MQTT client for status and control, and `ffmpeg` for camera frames, with RTSP forced to TCP transport so no UDP is needed.
- For pausing, the user's explicit or standing authorization, with the failure criteria agreed in advance.

## Workflow

1. **Read status over LAN MQTT.** Connect to the printer on port 8883 over TLS (self-signed certificate, so pin it or disable chain verification), sign in as user `bblp` with the LAN access code, subscribe to `device/<serial>/report`, and publish `{"pushing": {"sequence_id": "0", "command": "pushall"}}` to `device/<serial>/request` to get a full report. Report the fields the printer returns: `gcode_state` (IDLE, PREPARE, RUNNING, PAUSE, FINISH, FAILED), job name, `mc_percent`, `mc_remaining_time`, `layer_num` against `total_layer_num`, nozzle, bed and chamber temperatures against targets, `print_error`, and AMS tray contents. If a connection attempt fails with a TLS timeout, retry once before reporting the printer unreachable; that failure is transient through the tunnel.
2. **Grab a camera snapshot over RTSPS.** With LAN Only Liveview on, the chamber camera serves an H.264 stream at `rtsps://bblp:<access code>@<printer IP>:322/streaming/live/1` (1080p, 30 fps). The RTSPS client cannot use the proxy directly, so run a small local TCP forwarder that opens the connection through the proxy's CONNECT method, then pull one frame, for example `ffmpeg -rtsp_transport tcp -tls_verify 0 -i <url> -frames:v 1 -q:v 3 out.jpg`. Take snapshots only while a print is active if that is the user's rule.
3. **Judge print health from the snapshot and telemetry.** Healthy: the part is on the plate and adhered, layers stack cleanly, stringing is minor, and early first layers look flat and thin. Alert-worthy: spaghetti or a growing filament nest, a part detached, tipped over, or being dragged, an empty plate well past the first layers, a blob on the nozzle, an obvious layer shift, a non-zero `print_error`, a FAILED state, an unexpected PAUSE, nozzle or bed temperature more than 15 degrees below target while running, or percent and layer both unchanged since the previous check for the same job. Attach the snapshot to any alert and name the specific evidence.
4. **Monitor a running print by polling.** A workable pattern is a 20-minute interval check that reads status first, takes a snapshot only when the state shows an active print, and stays silent when nothing is printing, when the print is healthy, and when it finishes normally. Keep the previous percent and layer in a state file so a stall is detectable, and suppress a repeat alert for the same job and problem for at least an hour.
5. **Pause only a clear-cut visual failure** (obvious spaghetti, a fully detached or dragged part, an empty plate well into the print, a large nozzle blob) when the user has authorized it. Publish `{"print": {"sequence_id": "0", "command": "pause"}}` to `device/<serial>/request`, once. Borderline or telemetry-only problems get an alert, not a pause.

## Verify the Result

- A published MQTT command is not proof it took effect. After a pause, wait about 15 seconds, read status again, and report the pause as done only when `gcode_state` reads PAUSE.
- If status still reads RUNNING after a pause attempt, report the pause as failed and tell the user to pause in Bambu Handy right away. Control commands can be blocked when the printer is not in LAN or Developer Mode; on the contributing setup, pause acceptance in cloud mode had not been live-tested yet, so verification is what makes the claim honest.
- Check that a snapshot file was actually written and view it before describing what the camera shows.

## Limits

- No cloud camera path: Bambu Handy is phone-only, and Bambu's cloud liveview uses a proprietary peer-to-peer ticket system that refuses third-party clients (OrcaSlicer gets a 403 on cloud liveview while its cloud status works). LAN access through Home Link is the working route.
- Do not enable LAN Only Mode or Developer Mode to gain control. Both disconnect the printer from Bambu Cloud and break Handy.
- No starting prints, arbitrary G-code, temperature, speed, or light changes, and no resume or stop under a pause authorization. Resuming is the user's decision, and stop destroys the print.
- Timelapse recording is separate from liveview: the P2S has no microSD slot and records timelapses to a USB flash drive in the front USB-A port, which is not included in the box.

## Sources

- [Building a Public Bambu Lab P2S Live Dashboard](https://www.mattcharlton.co.uk/posts/2026-10-07-bambu/) — the LAN Only Liveview configuration and the local RTSPS and MQTT paths, verified against a P2S.
- [Bambu Lab forum: MQTT commands](https://forum.bambulab.com/t/mqtt-for-a1/50033?page=2) — the `pause`, `resume`, and `stop` JSON commands on `device/<serial>/request`.
- [bambu-p2s-bridge PROTOCOL.md](https://github.com/kevinlibaba/bambu-p2s-bridge/blob/HEAD/PROTOCOL.md) — P2S LAN protocol notes for MQTT state and control on port 8883 and the camera on port 322.
- [OpenBambuAPI](https://github.com/Doridian/OpenBambuAPI) — community documentation of the Bambu MQTT and cloud HTTP protocols.
- [ha-bambulab](https://github.com/greghesp/ha-bambulab) — the Home Assistant integration and its `pybambu` client, another working implementation of the same MQTT reports.
