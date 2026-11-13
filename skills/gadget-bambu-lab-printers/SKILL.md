---
name: gadget-bambu-lab-printers
description: >-
  Read Bambu Lab printer status (job, progress, temperatures, filament) over the local network with the printer's
  access code; on a P2S, grab chamber-camera snapshots and monitor a running print while cloud mode is kept; and,
  only with LAN Only mode and Developer Mode enabled by the owner, start an owner-approved print. Not Bambu
  Cloud sign-in, arbitrary G-code, firmware or configuration changes; never enable Developer Mode on the
  user's behalf.
---

# Bambu Lab Printers

Use this skill when the home has a Bambu Lab printer (X1, P, A or H series) on the local network and the user asks
about print status, filament, a camera check, monitoring, or sending a print. For Klipper-based printers, use the
Moonraker skill instead.

Two local paths exist, for different jobs. With LAN Only off and LAN Only Liveview on, the printer stays on Bambu
Cloud: status, camera snapshots and monitoring work, and Bambu Handy keeps working. Print starts need LAN Only
mode plus Developer Mode, which disconnects the printer from the cloud. Confirm which path applies before acting.

## Identify the Device

- Confirm the exact printer (model and serial) from LAN discovery or a user-confirmed address. Homes often
  have more than one; confirm which one a question is about.
- Get the printer's IPv4 address and 15-character serial number from the printer touchscreen or Bambu Handy.
  The P2S does not announce itself by name over mDNS or SSDP, so a network scan alone cannot identify it.
  Match the address from the touchscreen against the scan results.
- Confirm the model is a P2S before using the camera path. Other Bambu models differ: the P1 and A1 families
  stream low-rate JPEG frames instead of RTSPS, and those paths are not covered here.

## Guided Setup Check

Walk through these in order. For a missing layer, give the user the step instead of improvising around it.

1. **LAN access code.** The owner reads the access code from the printer's network settings screen and stores
   it with the host's other device credentials, in a local file with restricted permissions. Clients read it
   from there in-process. The code is never pasted into chat, logged, passed in a command-line argument, or
   put in a URL passed on a command line. Validate the printer's TLS identity with a configured or pinned
   trust path before sending the code; do not disable certificate validation.
2. **Cloud-kept mode, for status, camera and monitoring.** Set **LAN Only** to off and **LAN Only Liveview** to on
   on the printer touchscreen. Liveview is a separate toggle from LAN Only Mode: with Liveview on and LAN Only off,
   the printer stays on Bambu Cloud, Bambu Handy and its live video keep working, and the local camera stream opens
   on the LAN. Verify Handy live video after changing the setting, and revert if it breaks.
3. **Developer Mode is the owner's decision, made knowingly, and only for print starts.** Starting prints
   from third-party tools requires LAN Only mode plus Developer Mode, enabled by the owner on the printer's
   touchscreen. Before they decide, state the trade plainly: while it is on, Bambu Handy, cloud printing,
   cloud print history and online firmware updates stop working; Bambu Studio on the same network still
   prints, and switching the mode off restores cloud features, though prints made in LAN mode are not added
   to the cloud history. Never enable it on the user's behalf, and never treat a question about printing as
   consent to switch modes.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- The guided setup check passes for the path being used (LAN status, camera and monitoring, or Developer-Mode print start).
- For a print start: a file the user explicitly selected, already sliced for this exact printer model, plus the user's
  confirmation that the build plate is clear and the printer is ready. Do not substitute another file.
- For pausing: the user's explicit request for that job, or a standing authorization the user gave for
  monitoring with the failure criteria agreed in advance.

## Workflow

1. **Status over LAN MQTT.** Connect to the printer on port 8883 over TLS, validating its certificate
   against the pinned trust path, and sign in as user `bblp` with the LAN access code read from the
   credential file. Subscribe to `device/<serial>/report`, and publish
   `{"pushing": {"sequence_id": "0", "command": "pushall"}}` to `device/<serial>/request` to get a full
   report. Report the fields the printer returns: `gcode_state` (IDLE, PREPARE, RUNNING, PAUSE, FINISH,
   FAILED), job name, `mc_percent`, `mc_remaining_time`, `layer_num` against `total_layer_num`, nozzle and
   bed temperatures against targets, `print_error`, and AMS tray contents (type, color, remaining amount)
   when an AMS is present, or the external spool otherwise. The P2S reports no chamber temperature. If a
   connection attempt fails with a TLS timeout, retry once before reporting the printer unreachable; that
   failure is transient through the tunnel. A certificate mismatch is not transient: stop and report it. Treat a
   disconnected printer as unavailable, not idle.
2. **Camera snapshot over RTSPS (P2S only).** With LAN Only Liveview on, the chamber camera serves an H.264
   stream over RTSPS on port 322 at path `/streaming/live/1` (user `bblp`, password the access code; 1080p,
   30 fps). The RTSPS client cannot use the proxy directly, so run a small local TCP forwarder that opens
   the connection through the proxy's CONNECT method. TLS still runs end to end between the client and the
   printer, so validate the printer's certificate against the pinned trust path rather than the forwarder's
   local address. Pull one frame with the credential-aware client: for example, a short script that reads
   the access code from the credential file, builds the stream URL in memory, and opens it with an
   FFmpeg-based library using `rtsp_transport=tcp` and certificate verification on. Never pass a
   credential-bearing URL to a command such as `ffmpeg -i`. Write each snapshot to a new file; take snapshots
   only while a print is active if that is the user's rule.
3. **Judge print health from the snapshot and telemetry.** Healthy: the part is on the plate and adhered,
   layers stack cleanly, stringing is minor, and early first layers look flat and thin. Alert-worthy:
   spaghetti or a growing filament nest, a part detached, tipped over, or being dragged, an empty plate well
   past the first layers, a blob on the nozzle, an obvious layer shift, a non-zero `print_error`, a FAILED
   state, an unexpected PAUSE, nozzle or bed temperature more than 15 degrees below target while running,
   or, while RUNNING, percent and layer both unchanged since the previous check for the same job. Attach a fresh
   snapshot to any alert when one can be taken, otherwise report the telemetry alone, and name the specific evidence.
4. **Monitor a running print by polling.** A workable pattern is a 20-minute interval check that reads status first,
   takes a snapshot only while `gcode_state` is PREPARE or RUNNING, and stays silent when nothing is printing, when
   the print is healthy, and when it finishes normally. A change to FAILED, an unexpected PAUSE, or a non-zero
   `print_error` always alerts, even though the print is no longer active. Keep the previous percent and layer in a
   state file so a stall is detectable, and suppress a repeat alert for the same job and problem for at least an hour.
5. **Pause only a clear-cut visual failure** (obvious spaghetti, a fully detached or dragged part, an empty
   plate well into the print, a large nozzle blob) when the user has authorized it. Publish
   `{"print": {"sequence_id": "0", "command": "pause"}}` to `device/<serial>/request`, once. Borderline or
   telemetry-only problems get an alert, not a pause. Pause acceptance in cloud mode has not been confirmed:
   always verify after pausing (see below), and never present a cloud-mode pause as working until it is.
6. **Print start (Developer Mode only), each step from a fresh status read:**
   - Confirm the printer is idle with no active or paused job and no error.
   - Confirm the loaded filament (AMS tray or external spool) matches the file's material. If the printer cannot
     report it, as with an external spool, ask the user to confirm the loaded filament instead of assuming it.
   - Confirm with the user this exact file, this exact printer and a clear build plate.
   - Upload the selected file over the local file channel, then send one documented start request for it. If
     any check fails or is unclear, stop and report.
   Report the start with the slicer's own estimates (filament, time) and the exact file and printer used,
   not guesses.

## Verify the Result

- A successful request is not proof that printing started. Re-read status: it should show printing with the
  expected job name and progressing layers.
- If the printer stayed idle or the result is uncertain, report it and inspect the current job. Never resend
  a start automatically, and never restart a running print.
- A published MQTT command is not proof it took effect. After a pause, wait about 15 seconds, read status again,
  and report the pause as done only when `gcode_state` reads PAUSE. If status still reads RUNNING after a pause
  attempt, report the pause as failed and tell the user to pause in Bambu Handy right away. Control commands can
  be refused when the printer is not in LAN or Developer Mode.
- Before describing or attaching a snapshot, check that the client exited successfully and wrote a new file
  for this request, then view it. Never describe an older image as the current view.

## Limits

- No Bambu Cloud sign-in or cloud print dispatch. Bambu authorizes cloud jobs itself; do not route around
  that authorization.
- No cloud camera path: Bambu Handy is phone-only, and Bambu's cloud liveview uses a proprietary
  peer-to-peer ticket system that refuses third-party clients (OrcaSlicer gets a 403 on cloud liveview while
  its cloud status works). LAN access through Home Link is the working route.
- No arbitrary or raw G-code, firmware or configuration changes, calibration or homing moves, or bypassing
  thermal and motion protections.
- Pause, stop or resume only on the user's explicit request for that job, or under a standing monitoring
  authorization with criteria agreed in advance. Intervening in someone else's print wastes filament and can ruin the part.
- No starting prints outside Developer Mode, and no resume or stop under a pause authorization. Resuming is
  the user's decision, and stop destroys the print.
- Timelapse recording is separate from liveview: the P2S has no microSD slot and records timelapses to a
  USB flash drive in the front USB-A port, which is not included in the box.
- Developer Mode relaxes Bambu's LAN authorization; it belongs on a trusted home network only.
- Bambu can change firmware authorization or LAN behavior at any time; an update may close paths this skill
  relies on.

## Sources

- [Bambu Lab Wiki](https://wiki.bambulab.com/)
- [Bambu Lab](https://bambulab.com/)
- [Building a Public Bambu Lab P2S Live Dashboard](https://www.mattcharlton.co.uk/posts/2026-10-07-bambu/) — the LAN Only Liveview configuration and the local RTSPS and MQTT paths, verified against a P2S.
- [Bambu Lab forum: MQTT commands](https://forum.bambulab.com/t/mqtt-for-a1/50033?page=2) — the `pause`, `resume`, and `stop` JSON commands on `device/<serial>/request`.
- [bambu-p2s-bridge PROTOCOL.md](https://github.com/kevinlibaba/bambu-p2s-bridge/blob/HEAD/PROTOCOL.md) — P2S LAN protocol notes for MQTT state and control on port 8883 and the camera on port 322.
- [OpenBambuAPI](https://github.com/Doridian/OpenBambuAPI) — community documentation of the Bambu MQTT and cloud HTTP protocols.
- [ha-bambulab](https://github.com/greghesp/ha-bambulab) — the Home Assistant integration and its `pybambu` client, another working implementation of the same MQTT reports.

> P2S camera, monitoring and failure-pause sections contributed by @sidhanthp, verified on a real P2S in cloud mode; Handy live video confirmed working alongside the LAN camera.
