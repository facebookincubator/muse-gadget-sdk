---
name: gadget-roborock-body
description: >-
  Pilot a compatible Roborock vacuum as a movable body: streamed remote
  control, segment-by-name navigation, room-to-room patrol, map and
  obstacle-photo eyes, and live camera over the account MQTT channel.
  Companion to gadget-roborock-vacuums, which owns cleaning and status
  over the local channel; this skill covers piloting and sensing where
  that local-only skill stops.
---

# Roborock Piloting, Map Eyes, and Live Camera

Use this skill when the task is to move a compatible Roborock vacuum
to a place, look at a room through its map, obstacle photos, or live
camera, or patrol room to room — rather than to start a clean.

Cleaning, pause, dock, and local status live in
`gadget-roborock-vacuums`. That skill uses the encrypted local TCP
channel. This skill adds two things on top of it:

- Piloting and movement verification, using the same command family.
- Sensing that the local-only lane does not provide: map retrieval,
  obstacle photos, and live camera, which run over the account /
  MQTT channel (the python-roborock cloud channel).

## Identify the Device

- Confirm the model is a Roborock-app-compatible vacuum supported by
  python-roborock, and record its firmware version. Behaviour below
  was verified on a Qrevo-family device (QX Revo Ultra 2) on current
  firmware; other generations may differ.
- Confirm both channels before relying on them: the local channel
  from `gadget-roborock-vacuums` for status and movement commands,
  and an account-authenticated MQTT session for map, photo, and
  camera RPCs.
- Resolve room segments by name from a fresh map parse. Segment IDs
  are deployment-specific; never hard-code another deployment's IDs
  or map coordinates.

## Prerequisites

- Follow the Home Link networking and safety rules in
  `~/docs/devices/home_link.md` where Home Link is the transport.
- Existing local key, device UID, and account credentials, supplied
  securely. Never print, log, or transmit credential values.
- The robot starts docked or idle, with healthy battery and no error
  state, read on a fresh status connection.
- A prepared movement area and a supervisor able to stop the robot.
  Piloting is explicit-invocation only, never ambient or scheduled
  unattended movement.

## Safety

- Load the stop path before driving: `app_rc_stop` while in remote
  control, or `app_stop` during any other mission. Stop first and
  diagnose second whenever motion looks wrong.
- Supervise every outing. Keep speeds low indoors.
- End every outing docked and charging, verified as below, unless
  the user directs otherwise.

## Workflow — Movement

### Go to a named room (preferred)

1. Send a segment clean for that room only, by segment ID resolved
   from its name on a fresh map, for example one segment for
   "Kitchen" or one for "Living room":
   `app_segment_clean` with `{"segments": [<id>], "repeat": 1}`.
2. This is the trustworthy go-to-room route: the robot navigates to
   the named segment reliably. It cleans while it is there — pause
   on arrival if the mission is presence, not cleaning, then stop
   before the next hop.
3. Arrival is a fresh status read showing room cleaning for that
   segment, plus a fresh map pose inside the room.

### Go to a point (use with caution)

- `app_goto_target` with map coordinates navigates autonomously,
  but targets derived from a stale map parse are unreliable: the
  robot may stop at a room entrance and give up. Prefer segment
  navigation, or short hops in open floor from a fresh map.

### Manual drive — streamed remote control only

1. Start remote control with `app_rc_start`.
2. Stream `app_rc_move` commands continuously, about 2–3 per
   second, each with an incrementing `seqnum` and the requested
   velocity, angular velocity, and duration.
3. End with `app_rc_stop`, then `app_rc_end`.

A single `app_rc_move` is a no-op: it returns success but produces
zero motion. The channel is a dead-man's switch — the robot only
drives while move commands keep streaming, joystick-style.
`app_rc_stop` interrupts a streamed move promptly.

### Patrol

- Patrol is a room-to-room segment tour: segment navigation to each
  room in turn, pause and sense on arrival, stop, then the next
  room, finishing with a return to dock.
- Do not rely on the native patrol command. `app_start_patrol` can
  reject with error -10007 when no cruise points have been
  configured in the vendor app. Until app-side cruise points exist,
  the segment tour is the patrol.

### Return home

- Send `app_charge` and wait for the docked state below.

## Verify the Result

Verification discipline applies to every movement claim:

- A command acknowledgement (`["ok"]`) is not movement. Verify with
  fresh status reads on new connections and with map pose: compare
  the vacuum position before and after the drive.
- State numbers from python-roborock status: returning to dock
  (state 6) is not docked. Only charging (state 8) closes a run.
  Report a run complete only on a fresh read of state 8 with no
  error.
- Inspect state before repeating a command after an uncertain
  response. Do not retry a movement blind.

## Eyes — Map and Obstacle Photos

- A fresh map parse gives rooms, vacuum pose and heading, charger
  position, path, and `obstacles_with_photo` entries (obstacle type
  and position). Fetch the map over the account / MQTT channel.
- Obstacle photos use python-roborock's obstacle-photo capability:
  1. Call `discover_features()` on the device properties first. The
     obstacle-photo trait only materializes after feature discovery;
     a flow that only refreshes known traits never creates it.
  2. Read photo IDs from the map's obstacle details: each
     `obstacles_with_photo` entry carries its ID in its photo-name
     detail.
  3. Check the trait is enabled, then fetch the photo by ID over
     the blob channel. Photos are small JPEGs of the obstacle as
     logged during a clean.
- Known parser bug and workaround: `vacuum_map_parser_roborock`
  version 0.1.5 and earlier only reads the photo name for 28-byte
  obstacle entries. Current firmware writes 36-byte entries, with
  the same 16-character photo ID at offset 12 in the entry. Parsers
  that return no photo name on current firmware are hitting this
  bug, not a device limit. Workaround: extract the ID directly
  from the raw map entry — 16 ASCII characters at offset 12 of the
  36-byte entry — and pass that ID to the photo fetch.
- Photos exist only for obstacles the robot has logged on the
  current map. There is no on-demand snapshot from this channel;
  the camera photographs obstacles during cleans.

## Eyes — Live Camera (WebRTC)

Live view is WebRTC. Signalling runs over the account MQTT RPC
channel, not the local channel:

- `START_CAMERA_PREVIEW` — open a preview session.
- `GET_DEVICE_SDP` — poll for the device's SDP answer.
- `GET_DEVICE_ICE` — poll for the device's ICE candidates.
- `GET_TURN_SERVER` — TURN server and credentials for the session.
- Close the session with the matching stop-preview RPC when done,
  and confirm no preview session remains connected.

### Camera credential

The preview is gated by the camera pattern set in the vendor app:

1. Convert the pattern to a digit string in draw order, using this
   grid:

   ```
   1 2 3
   4 5 6
   7 8 9
   ```

   For example, a stroke across the top row then down the left
   side is the top-row digits followed by the digits below the
   start corner, in the order drawn, with no separator. Include
   intermediate points the pattern UI inserts when a stroke
   crosses them.
2. The password is the lowercase hex MD5 of that digit string,
   with no salt and no device identifier.
3. Verify the encoding before opening any preview:
   `CHECK_HOMESEC_PASSWORD` with that password returns `["ok"]`
   when it is correct.

Never guess patterns or probe password shapes. Repeated invalid
attempts may lock the device, so validate once with the check
above and stop if it fails.

### Client reality

- go2rtc's Roborock producer works end-to-end on current Qrevo
  firmware: it performs the MQTT signalling, sends its offer
  immediately, trickles candidates to the device, and receives
  media. A still frame is available from its frame endpoint
  (`/api/frame.jpeg` for the configured stream), and its video
  endpoints carry moving video.
- A Python aiortc flow stalls on current firmware and is a known
  limitation, not a recommendation: the password check, preview
  start, TURN lookup, and offer acceptance all succeed, but the
  device never publishes its SDP answer to that client, so the
  SDP and ICE polls return their retry placeholder indefinitely.
  Prefer the working go2rtc client.
- Only one preview session can exist at a time. Check the
  connection status before starting a new one.

### Double-hash trap

go2rtc hashes whatever PIN it is given with MD5 before the
password check. Tooling that stores only the derived credential
hash (see below) must therefore pass that hash through unhashed,
or the check will fail on a double hash. A practical rule for a
patched or wrapping client: treat a PIN that is already a
32-character lowercase hex string as pre-hashed and use it as the
password as-is; hash anything else exactly once.

## Credential Hygiene

- Prefer storing only the derived MD5 hash of the camera pattern,
  never the pattern or its digit string. Store it in a file with
  mode 0600, and read it in-process only: never place it on a
  command line, in a log, or in a report.
- Put transient stream configurations that carry credentials in
  temporary files with restrictive permissions, and shred them
  after use.
- Rotate the stored hash only from a pattern the user supplies,
  in a dedicated handling, never from a guess.

## Limits

- Piloting moves do not clean, except segment navigation, which
  cleans its segment while travelling — pause it on arrival for
  presence-only missions.
- The camera is at ankle height, with no microphone and no
  face or presence detection. Obstacle sensing does not reliably
  identify people or pets; a room is located by clearing the other
  rooms and corroborating with other home evidence, not by the
  robot's obstacle types alone.
- Map, photo, and camera features depend on the account / MQTT
  channel and on current firmware behaviour; if that channel
  fails, report it rather than silently substituting another one.
- Not every Roborock generation supports the same commands,
  camera flow, or map entry layout. Confirm support on the
  specific model and firmware before relying on a step above.

## Sources

- [python-roborock device support](https://github.com/Python-roborock/python-roborock)
- [python-roborock camera protocol notes](https://github.com/Python-roborock/python-roborock/blob/HEAD/docs/CAMERA_PROTOCOL.md)
- [go2rtc Roborock producer](https://github.com/AlexxIT/go2rtc)
