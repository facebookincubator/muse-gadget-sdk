<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# muse_gadget_ble

**Experimental:** BLE gadget support is experimental while the Muse apps
gate it.

The device side of the musegadgets BLE protocol
([`protocols/mgcommands.h`](../../../protocols/mgcommands.h)): a phone,
browser or gateway connects over BLE, turns on push-to-talk, and gets each
utterance as SBC or LC3 frames. It's off unless `CONFIG_MUSE_GADGET_BLE_AUDIO`
is set; the gadget overlays turn it on (`tools/muse/board.sh build
s3-gadget` / `watcher-gadget`: `devices/sdkconfig.muse-<board>-gadget` after
the board's own), as do the C6's BLE-only overlay and the BLE video builds;
the boards' own overlays alone are Wi-Fi Muse builds. The gadget's device
tokens come from Muse Link setup ([`protocols/README.md`](../../../protocols/README.md),
Gadget setup over Muse Link), and the token proof checks on each connection
that the app and the gadget hold the same one.

## Files

| File | What it does | Host-tested |
|---|---|---|
| `mg_proto.c` | The protocol for one connection, with no BLE stack in it: commands, settings, capture (and its outbox, `mg_capq_t`), gestures, the clip queue commands, playback and the token proof | `test_mg_ble`, `test_mg_token_proof` |
| `mg_session.c` | The device session (`mg_command_session`) for one connection, with no BLE stack in it: hello and the credentials after it, the client's session state, messages both ways with credit and abort, `credentials_update`, and the JSON both ways (`include/mg_session.h`) | `test_mg_session` (the JSON vectors) |
| `mg_token_proof.c` | The token proof's crypto on PSA Crypto: K by HKDF-SHA256, the HMAC-SHA256 tags, a constant-time compare (`include/mg_token_proof.h`; the host tests use `tests/mg_token_proof_ref.c`) | `test_mg_token_proof` (the JSON vectors) |
| `mg_codec.c` | SBC and LC3 encoders at 16 kHz mono, whole frames, `change_data_type` parameters | `test_mg_codecs` |
| `mg_queue.c` | The offline clip queue on a flash partition, safe against resets | `test_mg_ble` |
| `mg_route.c` | Where a press goes: BLE, Wi-Fi, the queue, or nowhere | `test_mg_ble` |
| `mg_play.c` | Playback: the ring the client's frames go into (flow control, prebuffer, underruns, drain and drop), the decoder (SBC, LC3, PCM to 16 kHz), and the drift trim | `test_mg_play`, `test_mg_ble`, `test_mg_ble_client` |
| `mg_chain.c` | A board's speech chain on the player's output (the ESP-Mosaico's): when it runs, drains and empties around a reply's end, a flush, a drop and the barge-in pause | `test_muse_spk_dsp` |
| `components/muse_vstream` | Video: the ring Data writes go into during a stream and the frame reader the decoder pulls from, with resync after lost bytes (`muse_vstream.h`; the stream's parameters in `include/mg_video.h`) | `test_mg_ble` |
| `mg_ui.c` | UI input with no BLE stack in it: touch samples to down / move / up events, the square-to-disc warp and its inverse, `hid_map`, the HID report map and reports, the relative pointer's park-and-step moves | `test_mg_ui`, `test_mg_ble` |
| `mg_hogp.c` | HID over GATT (`CONFIG_MUSE_GADGET_BLE_HID`): the HID service, bonding, the host's subscriptions | built only |
| `mg_resample.c` | Rational polyphase resampler (8, 32, 44.1 and 48 kHz to 16 kHz), integer arithmetic | `test_mg_play` |
| `mg_ble.c` | NimBLE: the GATT services, GAP events, a worker task, NVS (settings), the Nordic UART log mirror, Battery and Device Information | built only |
| `mg_voice.c` | The voice transport layer the push-to-talk loops call (`include/mg_voice.h`) | built only |
| `mg_smp_esp.c` | The ESP-IDF backend under the vendored MCUmgr SMP core (`smp/`): the other OTA slot, the board check on the app's first bytes before any write, the image state, the rollback verdict | `test_mg_smp` |

The codecs are vendored once for both SDKs, in
[`../../../xplat/libsbc`](../../../xplat/libsbc) and
[`../../../xplat/liblc3`](../../../xplat/liblc3) (Google's,
Apache-2.0); [`../libsbc`](../libsbc) and [`../liblc3`](../liblc3) build them.

## How it fits with Home Link's BLE

Home Link owns the NimBLE host (`main/ble_server.c`), for setup, community
pairing and provisioning, and Muse's phone setup service already shares it as
a *companion*. This is a second companion (`main/mg_glue.c` registers it):

- **One host, one GATT database.** Its services join Link's at startup, and it
  sees every GAP event. There's one connection at a time, so while an mg client
  is connected nothing else can connect, and the other way round.
- **Advertising.** Legacy advertising holds one 128-bit UUID. Link's server
  advertises the musegadgets UUID (name in the scan response) whenever this
  wants it: after setup, that's always while no one's connected, fast
  (30-50 ms) for 30 s after boot, a disconnect or a talk press with no app
  connected, then 417.5 ms (`main/adv_pace.h`); before
  setup, 100-150 ms. While Link's own setup advertising is also on (unpaired, or Muse's
  phone setup switched on), the two payloads take turns every 1.5 s, so both
  apps find the device. Clients can also filter on the `MuseGadget` name.
- **Link modes (mgcommands.h "Link modes",
  `mg_linkpol.h`).** Several gadgets share one phone's radio and the central
  serves every link at its interval, so only the gadget in use holds a short
  one. Three modes, each a set of connection parameters (interval min/max in
  1.25 ms units, latency, timeout in 10 ms units): *sleep* 100/160/4/600
  (125-200 ms), *ready* 24/36/5/400 (30-45 ms), *active* 12/12/0/400 (15 ms;
  Apple grants 15 ms only for exactly 12/12).
  - **Ready at every connect**, asked after the central's own first update,
    or 1 s after connect if none came (a request colliding with the
    central's update can go unanswered and the controller drops the link
    40 s later).
  - **Active on the gadget's own activity**: its talk press (at touch-down),
    a call (from Calling... on), a stream the app starts (video, a UI stream
    while its picture changes, audio playback, a clip transfer or throughput
    test), push-to-talk capture, Link setup (a pairing session, and any
    connection while the gadget isn't set up) and an MCUmgr update. A touch,
    a key, a menu, the console or a GATT write alone isn't activity.
  - **A user's wake or touch moves sleep to ready** (mgcommands.h):
    any key press, a touch, the screen lit by the user, and the bench's
    injected input alike, go from sleep to ready (never active) and restart
    the 10 min in ready; a sleep the app wrote is ready again until 10 min
    after the last one, then sleep. A call from sleep otherwise waited
    11.75 s for the central to schedule the active update (a gadget and an S22).
    A talk press still goes straight to active. Logged `link mode sleep ->
    ready (touch)` (`key`, `wake`).
  - **The gadget's own turn is activity** (mgcommands.h): from its talk
    press (the press the client heard,
    `mg_ble_link_turn()` at button_down) until its reply has played (the
    reply stream that started after the press stopped: closed, or in a Live
    call's one stream no longer audible), or assistant_state done or error,
    or 20 s with no reply (`MG_LINKPOL_TURN_WAIT_US`, counted from the
    capture's end). On the Watcher, without it, the policy dropped to ready 5 s
    after the release while Muse was still thinking, and the reply started
    8 s later on the ready link (active re-asked, granted after 1.0 s; 6
    underruns). Logged `link mode active kept (turn: waiting for the reply)`
    once the turn holds it past the 5 s, and `link: turn over: its reply
    played` (`20 s with no reply`, `assistant_state done` / `error`) at its
    end; the 5 s count from there.
  - **Back to the app's mode 5 s after the last activity ends**; **sleep by
    itself after 10 min in ready** (until the next activity or the app's
    next write).
  - **The app's mode**: setting `link_mode` (16), per connection, ready at
    connect, never stored. A write of active makes the gadget active ahead
    of a stream and **expires to ready after 30 s without activity**; a
    write of ready or sleep takes effect once the gadget's own activity has
    ended (held until then: a read still returns active, and the
    notification shows the written mode when the activity ends); another
    value is refused (invalid_value). A read returns the
    effective mode, and every effective change is notified
    (`[get_settings, link_mode, mode]`). `link_params` (17, read-only,
    6 bytes) is the granted interval, latency and timeout, little-endian.
    `request_status` lists both on every musegadgets build.
  - **Requests**: each mode change asks its set once, unless the link
    already satisfies it (a central rejects the set it's in); any update the
    central makes by itself is accepted. A refused request (an L2CAP reject,
    a failed update, or the stack refusing to send it) is logged and asked
    once more after 5 s, then left until the next mode change. A refusal
    for a procedure collision (LL 0x2A, Different Transaction Collision;
    NimBLE's status 554: our request crossed the central's own update) is
    asked again after 300 ms instead (`MG_LINKPOL_COLLISION_US`), outside
    the 2 s pacing and the refusal's own settle, unless the link is in the
    set by then; twice a mode at most, then as any refusal. Logged `link:
    request refused (LL 0x2A, a procedure collision): asking again in
    300 ms`. A request with **no answer at all in 2 s** (no grant, no
    refusal) is asked again then, unless the link reached the mode's set
    meanwhile (on the Watcher a talk press from sleep asked active
    after a quiet night, nothing ever came back, and since only a refusal
    re-asked, the whole turn rode the 200 ms link). Logged `link: no answer
    to the request in 2 s, asking again`. NimBLE keeps an unanswered request
    for 40 s (`BLE_GAP_UPDATE_TIMEOUT_MS`) and ends it silently, with no
    connection update event, refusing another one meanwhile with
    `BLE_HS_EALREADY`: that isn't a refusal (`mg_linkpol_request_busy()`,
    logged `link: the stack still holds the unanswered request: asking again
    in 2 s`), and it's asked again every 2 s, so the first re-ask that NimBLE
    takes goes out as soon as its entry ends. Bounded: 45 s from a mode's
    first unanswered request (`MG_LINKPOL_UNANSWERED_MAX_US`), then as a
    refusal (once more after 5 s, then left until the next mode change). A
    late outcome still counts as the request's own. At most one
    request per 2 s, none while one is unanswered (up to 2 s), none within
    500 ms of any connection update. With the first request, 2M PHY and
    251-byte packets; the ATT MTU is 247 in the BLE gadget overlays (the
    video one included), 256 in other full-UI builds.
  - **Call-start toggle** (kept with the modes): a call or video
    stream that starts while already active toggles idle -> fast so the
    central re-plans the link: the idle half 24/36/0/400, then, once its
    connection update event arrives (granted or refused), the fast half no
    sooner than 500 ms after it (a gadget and an S22: a fast half asked 1 ms
    after the idle half's grant lost the link at its instant) and 2 s after
    the idle half; if the idle half gets no answer in 2 s the toggle is
    skipped for that call (logged) and the fast set stays. A mode change in
    between supersedes it, a disconnect cancels it.
  - The screen and USB power don't matter (no mode follows the screen or
    USB power).
  - The worker runs the policy; a one-shot timer wakes it when its next round
    falls due (a paced, settling or retried request, the 5 s, the 30 s, the
    10 min), so the NimBLE host task only records the update event. Latency
    0 in active, and the listening window below during each switch, are the
    mechanism; the C6 also turns on its controller's fast reception after a
    data packet (`CONFIG_BT_LE_CTRL_SLV_FAST_RX_CONN_DATA_EN`).
  - **Listening at every event during a switch** (on a Nordic XIAO
    with a Mac central: sleep -> active median
    6.8 -> 3.4 s). From each request until its set applies (the grant), a
    refusal, the stack refusing to send it, the link's loss, or 20 s after
    the request (`MG_LINKPOL_WAKE_US`; the switch's request: its re-asks of
    an unanswered request neither extend nor reopen it), the peripheral ignores its slave
    latency locally and wakes at every connection event, so the central's
    answer and its LL_CONNECTION_UPDATE_IND are heard at the next event
    rather than after up to latency + 1 intervals (sleep: up to 1.25 s per
    exchange). Only while the link in effect has a latency (sleep's 4,
    ready's 5): nothing to ignore at 0, so the toggle's halves and active
    -> ready open none. The policy opens and closes the window
    (`mg_linkpol_wake_sync()`, pure, `tests/test_mg_linkpol.py`); the
    mechanism depends on the controller (`link_wake_sync()` in `mg_ble.c`):
    - **ESP32-S3 and C3**, experimental (Espressif's RW-based BTDM
      controller, `libbtdm_app.a`, checked in ESP-IDF v6.0.3: the handler
      and `lld_con_pref_slave_latency_set` are still in it): the
      undocumented vendor command
      **0xFC13, Set Preferred Slave Latency** (connection handle, latency;
      returns status and handle). It isn't in `esp_bt_vs.h`, but the
      controller has it in its HCI descriptor table (`hci_cmd_desc_tab_vs`,
      `hci_msg.o`: format `HH` -> `BH`) and its LLC handler table
      (`llc_hci_command_handler_tab`, `llc_hci.o`), handler
      `hci_vs_set_pref_slave_latency_cmd_handler` ->
      `lld_con_pref_slave_latency_set` (also in the S3 ROM,
      `esp32s3.rom.bt_funcs.ld`: 0x40002bec and 0x400042c0). It sets the
      latency in use to min(in use, preferred), so 0 wakes at every event.
      **Its limit:** it can't raise it again. The controller reloads the
      granted latency only at a connection update instant
      (`lld_con_sched`) or a new connection, so a grant (or the central's
      own update) restores it by itself, but after a refusal, the stack
      refusing to send, 20 s or `link.wake=0` the gadget keeps listening at
      every event until the next connection update: logged `link:
      listening every event until the next connection update (N ms;
      refused: the controller can't lift its override)`, then `link:
      latency back (the controller's override ended at the connection
      update)`. A refusal is asked once more after 5 s (a collision's after
      300 ms), which usually ends it; after a second refusal it lasts until
      the next mode change's grant (in ready, listening every 30-45 ms
      instead of every 180-270 ms). Sent with NimBLE's own HCI call
      (`ble_hs_hci_cmd_tx`, no response buffer: a return length NimBLE
      didn't expect would reset the host); a refused command (status) falls
      back to the pending notification for the rest of the boot. Not on the
      classic ESP32's controller.
    - **The others** (C5, C6, C61, H2 and the S31's controller,
      `libble_app.a`): no latency override (no vendor command, no API; the
      C6's `BT_LE_CTRL_SLV_FAST_RX_CONN_DATA_EN` is build-time, for the
      whole controller, and only after a received data PDU). A peripheral
      with data queued transmits at every event whatever its latency, so a
      notification is queued every half interval in effect
      (`mg_linkpol_wake_period_us()`, at least 3.75 ms): the Battery
      Level's current value if the client subscribed to it (a valid,
      idempotent report), else an empty Nordic UART TX notification if
      subscribed, else nothing (logged once a window: `link: nothing
      subscribed to keep a notification pending: latency kept`); never a
      notification the client didn't subscribe to. Cost: a 1-byte
      notification is a 12-byte encrypted LL payload, 23 bytes on air,
      92 us at 2M (184 us at 1M), two per interval: 10 a second at sleep's
      200 ms (a 3.4 s switch: ~34, ~3 ms of transmit), 44 a second at
      ready's 45 ms; the radio is up for those events anyway, since
      listening at them is the point, and the worker sends them
      (`link_wake_tick`). Whether the Muse apps subscribe to Battery Level
      or the Nordic UART on these boards isn't checked here.
    Logged on `mg.ble`: `link: listening every event (switch to active)`
    when one opens, `link: latency back (812 ms): granted, latency 0` (with
    the notifications sent on the notification path) when it closes.
    Bench switch: `link.wake=0|1` (off by default: it goes on only
    where an A/B shows a gain, and neither path showed one: talk press
    from a sleep link to the active grant, S31 notify fallback + iPhone 17
    2976 vs 2974 ms, S3 0xFC13 + Galaxy S22 10.7 vs 11.1 s, n=3+3 each;
    `CONFIG_MUSE_GADGET_BLE_LINK_WAKE` turns it on by default; RAM only, kept across
    connections; off closes an open window) and `link.wake?` (the switch,
    the mechanism, the window), on the USB console (`>link.wake=0`,
    answered `@link.wake ...`) and as plain Nordic UART lines (in a release
    build the switch needs a proven connection there; the question
    is a read). Not measured on a board
    yet (the bench's A/B: `link.wake=0` vs `1`, sleep -> active on the same
    phone and gadget).
  - Sleep set (bench, for a sleep-parameter A/B):
    `link.sleep=ITVL_MIN,ITVL_MAX,LAT` (1.25 ms units) replaces sleep's
    100/160/4 in the sleep mode's request (`mg_linkpol_sleep_word()`); the
    timeout stays 6 s. RAM only, kept across connections like `link.wake`
    and the latency knob; a restart or `link.sleep=0` restores the defaults.
    A set the BLE spec refuses (interval 6-3200, min <= max, latency 0-499,
    6 s > (1 + latency) x max x 2) gets the usage line and changes nothing;
    one outside Apple's rules is taken with a warning (an iPhone may refuse
    it). A change while the link is in sleep asks the new set at once
    (paced as any request). `link.sleep?` answers the set in effect and the
    last sleep grant (`link.sleep 160-320/2/600 (link.sleep; 200.00-400.00
    ms, latency 2, 6000 ms timeout), last sleep grant 320/2/600 (400.00 ms,
    latency 2) 10 s ago`). Same consoles and switch as `link.wake`: setting
    is automation, the question a diagnostic. `test_mg_linkpol` runs it.
  - Logged on `mg.ble` (on the Nordic UART mirror): each mode change with
    its reason (`link mode ready -> active (talk press)`), each request
    (`link: asked 12-12 x1.25 ms, latency 0, timeout 4000 ms (active: talk
    press) rc=0`), each grant (`link: granted 15.00 ms latency 0, timeout
    4000 ms`) or refusal, the toggle's outcomes and the app's writes, for
    a record of what iPhone, Android and Mac give.
- **Off the phone for a while (bench).** `>ble.pause=SECS` on the USB
  console, or `!IN ble.pause=SECS` on the Nordic UART,
  drops the connection and stops all advertising for SECS (1-600), then
  advertises as before: a gadget-side "off the phone" for A/B tests. Nothing
  else changes: no unpair, no NVS write, a restart ends it; it counts as
  reachable on purpose meanwhile. Logged on `mg.ble`; the link drops
  300 ms after the word, so the line reaches a Nordic UART client first.
- **The stack stays up.** Link normally shuts BLE down once setup is done
  (full-UI boards with PSRAM keep it idle for phone setup). With this on, it
  starts the server after setup too and only stops setup advertising.
- **Security doesn't mix.** Link's setup commands keep their own encryption
  and refuse to run once set up. Muse's phone setup keeps requiring an
  authenticated bonded link. The musegadgets characteristics need no BLE
  pairing, as the protocol says, and aren't encrypted: the token proof says
  the gadget holds the token the phone set up, not that the link is secure.
- **One address.** Link setup and the musegadgets UUID advertise from the
  same public address (one advertiser taking turns), which apps rely on to
  match the gadget they set up with the one they connect to.

## BLE only

`CONFIG_MUSE_GADGET_BLE_STANDALONE` turns a board into a musegadgets gadget
and nothing else, for boards without PSRAM (the Waveshare C6 profile `c6ble`)
or for testing the apps. `app_run()` hands over to `run_ble_standalone()`
before Wi-Fi: Wi-Fi, the Muse session, the tunnel and OTA never start, and
Muse's phone setup service and Wi-Fi keeper aren't registered. Link setup
runs without Wi-Fi (`device_info` says `wifi: "none"`): until the gadget is
set up it advertises setup, taking turns with the musegadgets UUID, and takes
community pairing v5 (the talk button confirms), token-only `provision_v2`,
`device_info` and `unpair`; `wifi_scan` answers `error_wifi_unsupported` and
OTA isn't there. Set up means the setup marker and the proof key; a boot that
finds anything less (a power cut midway, or a setup from Wi-Fi firmware)
erases it and starts setup again. Afterwards only the musegadgets UUID
advertises. The routing policy
is BLE only, the offline queue is on by default (a client can turn it off),
and without PSRAM a queued clip is staged in internal RAM, up to about 8 s of
SBC. The screen follows the client's state (`mg_ble_link()`, `mg_link.h`):

| State | Shown | When |
|---|---|---|
| never | `OPEN MUSE APP` | no client since boot and no proof key: nobody has set it up |
| disconnected | `DISCONNECTED` | no connection |
| connecting | `CONNECTING` | connected, not yet subscribed to Control and Data |
| session | `APP CONNECTED` | the client's session is up, push-to-talk off |
| ready | `READY` | ...and push-to-talk on: the only state with the mic and speaker icons, and a highlighted Bluetooth icon |

The state is re-read after every GAP event (connect, any disconnect,
subscribe or unsubscribe) and every command, and each change is logged as
`mg.ble: link <old> -> <new>`, with the label as `mg.face: label <LABEL>`,
both on the Nordic UART mirror. The gadget name stays on screen until a
client's session is up.

A phone that keeps the link and its subscriptions after the app is gone
(backgrounded on iOS, or held by the system) still looks ready: nothing on
the link says the app left. Telling that apart needs a liveness signal from
the client, which the protocol doesn't define yet.

## The voice transport layer

Push-to-talk loops (`components/muse/muse_voice.c` on boards with the full UI,
`main/voice.c` on the Voice PE) ask `mg_voice_route()` once per press:

| Policy (`CONFIG_MUSE_GADGET_BLE_ROUTE_*`) | Client ready and push-to-talk on | Else, Wi-Fi up | Else, queue on and no client connected | Else |
|---|---|---|---|---|
| Auto (default) | BLE | Wi-Fi | queue | Wi-Fi (it saves the note or says why not, as before) |
| BLE only | BLE | queue if on, else refused | queue | refused |
| Wi-Fi only | Wi-Fi | Wi-Fi | Wi-Fi | Wi-Fi |

"Ready" means connected and subscribed to Control and Data. Wi-Fi turns run exactly
as without this option. On BLE the loop streams the pre-roll and the mic into
`mg_voice_audio()` until release. A client's `start_mic` wakes the loop to
capture until `stop_mic`. Every capture ends with a `stop_mic` notification,
whichever side started or stopped it. Every press and release goes to a ready
client as a gesture, whatever the route.

**A capture on a slow link is held, never cut short** (the capture's outbox,
`mg_capq_t` in `mg_proto.h`; on the Watcher a talk press on a sleep
link, 200 ms latency 4, whose request for active went unanswered, lost 156 of
its frames, then its `stop_mic`, and the phone waited out its 120 s recording
timeout and swallowed the next press too). Before, the voice task sent each
slice itself and, with the stack out of buffers, waited 4 ms up to six times
and then dropped it (stalling the mic meanwhile), and `stop_mic` and the
gestures were sent once, lost if the stack was full. Now:

- Everything a capture sends goes through one queue, in order: `[gesture,
  button_down]`, `change_data_type`, the audio, `[gesture, button_up]` where it
  was released, the tail, `[stop_mic]`. A gesture outside a capture goes the
  same way. A gesture goes at once, ahead of held audio and queued replies,
  when nothing of the capture's control waits and the stack takes it
  (mgcommands.h); else it waits there in order and
  governs what follows. A turn's own frames pass replies waiting in the
  reply outbox, and a client command that would answer through that outbox
  waits (`mg_cmdq_t`) until it's empty, so replies keep their order and are
  never dropped (`test_ptt_ordering`).
- The audio is held in a buffer allocated at the capture's start: in PSRAM,
  `CONFIG_MUSE_GADGET_BLE_CAPTURE_HOLD_S` (10 s) of its format (75 KB of
  60 kb/s SBC); without PSRAM (the C6) in internal RAM, up to
  `CONFIG_MUSE_GADGET_BLE_CAPTURE_HOLD_INTERNAL_KB` (8 KB) while 32 KB stays
  free past it; else 512 bytes built in. It's given back once everything,
  the end included, has gone.
- It drains as fast as the link takes it: the voice task's each 20 ms slice
  and the worker's 5 ms rounds (5 ms while anything is held, and the link
  counts as busy with push-to-talk capture until it's empty), whole frames,
  as many as fit a notification. Held audio leaves the stack 4 msys blocks
  (`CAPQ_MSYS_KEEP`), so a control message always finds one. Nothing in it
  blocks the voice task.
- Control messages wait in place between the audio and are never dropped,
  only retried at every pump until they go or the link drops. Only when the
  buffer overflows is the oldest audio dropped, whole frames; a gesture or the
  end the dropped audio went past still goes, before the audio after it.
- A link that drops takes what's held with it (there's nobody to send it to).
  A clip download and the throughput test wait (busy) while a capture still
  holds audio, as during live audio.
- Logged on `mg.voice` (on the Nordic UART mirror): `capture over: 3.3 s,
  24780 bytes, 9120 still held for the link; its end follows them` when the
  mic stops, and `utterance over BLE: 3.3 s, 24780 bytes, 0 frames dropped,
  rms -24.1 dBFS, peak -3.2 dBFS, floor -61.0 dBFS, loud1s -19.8 dBFS, 0
  clipped; end sent 1900 ms after the capture ended, up to 2950 ms held for
  the link` when `stop_mic` went. The level (whether the mic heard
  speech, from the gadget alone) is measured as the Wi-Fi path's `muse_voice:
  recorded ...` line does: rms over every sample, peak, floor (the quietest
  20 ms window), loud1s (the loudest 1 s window, from each 10 ms half of that
  window: a running sum over a 100-frame ring, `mg_loud1s.h`; the whole
  capture under a second; left out without the ring) in dBFS, and samples at |v| >= 32000 (`mg_capture_meter_t`, integer sums on
  the voice task, dB at the end; `mg_proto_capture_level()` hands it over
  before the end is queued, so each summary keeps its own utterance's level
  when several ends wait).

`test_mg_ble`'s harness runs a 3.3 s turn on a modeled sleep link whose
request is never answered (every frame delivered in order, button_up where it
was released, `stop_mic` after the last frame, the request at the press and
again at 2 s), the overflow (the newest frames kept, the gestures and the end
never dropped, a full stack holding `stop_mic` until it has room) and a fast
link (each slice out as it's encoded, `stop_mic` at once, nothing held).

**The bench guard: a turn the mic didn't hear never leaves the gadget**,
so a turn whose capture never rose above the room's floor isn't sent. The
console word `ptt.minrise=N` (USB `>` and the
Nordic UART's `!IN`, which a release build takes only on a proven connection; N
1-40 dB) holds every push-to-talk turn the gadget starts on the gadget until
its capture rises N dB over its own floor; `ptt.minrise=0` turns it off and
`ptt.minrise?` (a diagnostic) answers `@ptt.minrise N` or `@ptt.minrise off`.
RAM only (`mg_voice.c`, `mg_minrise.h`): never stored, off at every boot, so
a user's quiet speech is never dropped; the test tools set it before spoken
rows. With it off nothing below happens: the turn goes exactly as before.

- The hold (`mg_proto_turn_hold`, asked by the voice task just before a BLE
  turn's `button_down`): everything the turn queues in the capture's outbox
  (`button_down`, `change_data_type`, the audio from the pre-roll on,
  `button_up`, `stop_mic`) waits there unsent, while what an earlier
  utterance still holds drains as usual. The outbox takes the whole turn:
  `MG_CAPTURE_GUARD_S` (16 s: the 15 s cap and the pre-roll) of its format in
  PSRAM (120 KB of 60 kb/s SBC, 64 KB of 32 kb/s LC3, 192 KB at LC3's 96 kb/s
  top; only while a held turn needs it), an earlier
  utterance's smaller buffer grown with what it holds; without PSRAM (the C6)
  up to `CONFIG_MUSE_GADGET_BLE_CAPTURE_GUARD_INTERNAL_KB` (32 KB, about 4.3 s
  of SBC) while 32 KB stays free, the oldest frames dropped (counted) past
  that. `request_status` keeps reporting the data type from before the turn.
  The turn's claim of the speaker waits too: a reply paused at the touch-down
  (the barge-in echo rule) stays held, neither dropped nor flushed.
- The rise is measured by the utterance summary's own meter
  (`mg_capture_meter_t`), after every slice: the loudest 1 s window (`loud1s`;
  the whole capture while it's under a second) over the floor (the quietest
  20 ms window since the turn's first sample, pre-roll included), in dB. Both
  only move one way (the loudest window up, the quietest down), so a turn
  once released stays released.
- At N or more the turn is released (`mg_proto_turn_rise`): its `button_down`
  goes, then its claim (`stop_streaming` drop, or a Live call's flush), then
  `change_data_type` and the held audio from the touch-down on, as a burst in
  whole frames per notification (at most MTU - 3, 4 msys blocks kept for
  control: the outbox's own pacing), then the live audio, `button_up` and
  `stop_mic` as for any turn. The link turn and the haptic interruption a
  `button_down` brings happen then too. Logged `ptt: rose 23.4 dB over its
  floor at 0.82 s (>= 15): turn released, N bytes held go first`.
- One that never rises is dropped at its end (`mg_proto_turn_drop`): nothing
  of it goes: no gesture, `change_data_type`, Data or `stop_mic` (a `stop_mic`
  without its `button_down` would end a recording the client never started).
  Logged `ptt: nothing heard (rise X dB < N): turn not sent` on `mg.voice`
  (the Nordic UART mirror), counted as `ptt_nothing=K` on the console's
  `status` line, and the face says "Didn't catch that" (idle, never
  thinking). The claim's off then lets a reply held since the touch-down play
  on from the very sample it paused at, as after a tap.
- A Live call's talk hold is the same turn: held, released with the call's
  flush, or dropped with the call's stream playing on.

`test_mg_ble`'s harness (`test_minrise`, `test_minrise_reply`) runs it off
(today's messages, bytes and instants exactly), a silent capture (nothing on
Control or Data, counted, the buffer back, `request_status` unchanged), a
rise at 0.8 s and a 3 s hold before the rise on a ready link, a sleep link and
the C6's 32 KB (every frame from the touch-down in order, 0 dropped, whole
frames within the MTU), the buffer grown under an earlier utterance, and a
reply or a Live call's stream at the touch-down (held, then dropped or
flushed after `button_down` on a rise, resumed on a drop).

**Haptics.** Only a board with a vibration motor (`mg_ble_platform_t.vibrate`)
lists `haptics_enabled` and the `ptt_buzz_*` settings and plays the
activation buzz: the M5Stack StopWatch (its ERM), through the haptics core
in `haptics/`, which also serves
`haptic_play` / `haptic_stop`; other boards answer `unsupported`.

**Vibration tests**: Settings > Developer > Tests >
Vibration (Developer mode: five taps on About's FW card) and the console's
`vib=pulse,MS[,PCT]`, `vib=preset,NAME[,PCT]`, `vib=stop` and `vib?` (USB
console `>vib=...`; the Nordic UART's `!IN vib=...`, or `vib=...` as a plain
line)
play test pulses and presets through the same core, gates, timer and motor
as a client's pattern (`haptics/mg_haptic_test.c`, `mg_ble_haptic_test*()`):
a test skips `haptics_enabled` and nothing else (update, battery, sleep,
talk turn, caps, both duty budgets), and answers each with the page's
status line on `mg.bench`. Only a tap or a console word starts one; any key
stops it.

**The Nordic UART console** has no owner unlock. In an automation build
every word works as sent: `status`, `version`, `guard?`,
`ota?`, every `!IN` word of the platform's allowlist (`voice?`, `wake?`,
`call?`, `vib=...`, `vib?`, `key=`, including talk holds that open the mic,
`ptt=`, `tap=`, `press=`, `page=`, `call`, `call=0`, `wake`, `ble.pause=`,
`session.ping`, `mic.gain?`, `mic.gain=`, `spk.dsp`, `spk.dsp?`, `spk.dsp=`, `ptt.minrise=`,
`ptt.minrise?`, `vol=`, `vol?`), `!PAIR AUTO` (the USB console's `>pair.auto` too),
`!RESTART`, `!SAFEID`, `!SILENCE` and `!SETUP ADV`, all open so benches
stay automatable (an armed `!PAIR AUTO` or an injected press can complete a
Link setup with no one there, a known and accepted trade-off). An old bench tool's `?UNLOCK`, `!UNLOCK ...` or
`!LOCK` gets `UNLOCK not needed` (`!LOCK` still drops a `!RESTART`
schedule). The allowlist stays: nothing that changes setup, power or tokens
beyond these. `CONFIG_MUSE_GADGET_AUTOMATION` (default n; on in bench builds,
`MUSE_BENCH=1` with `devices/sdkconfig.muse-bench`) keeps them open to any
connection. Bench images go on bench units only; a gadget someone wears or
carries runs a release image. Off (a release build, every board overlay),
the ones that change state run only on a
connection whose token proof matched and are refused otherwise
(`mg_unlock_remote_ok()`, logged as `locked`; in SAFE, `safe.off` / `safe=0`
stay open). There, arming `!PAIR AUTO` is refused on every connection (the
USB console's `>pair.auto` arms it), and a `key=` while a setup or re-setup
prompt is up answers `real press needed` and presses nothing. Identity and status reads (`status`, `version`, `guard?`,
`ota?`) stay open, and their answer goes back to the asking connection,
proven or not, SAFE included; so do the other `!IN` reads (`wake?` /
`call?` / `vib?` / `mic.gain?` / `spk.dsp` / `spk.dsp?` / `ptt.minrise?` / `vol?`).
The owner's content (`!IN voice?`, voice activity, and `!IN img?`, the
shown image's path) answers only a proven connection on a set-up release
gadget (`mg_unlock_owner_word()`); refused, it's logged on the gadget and
nothing goes back (the log mirror is silent to that link, Token proof
below). The USB console is open in every build, with every word. `request_status` ends
with the typed list `[supported_features, sub_feature, set_settings, ...]`
naming every setting the board accepts: `spec_version`,
`push_to_talk_enabled`, `audio_codec`, plus `audio_queue_enabled` with a
queue, `speaker_volume` with a speaker, `link_mode` and `link_params`
(link modes, above), and `client_app` (below; `client_name` is reserved).

## Who is connected (setting 19; 18 reserved)

The Status page names the phone and app, to help figure out which phone and
app the gadget is connected to. The phone's name is only the one the gadget
reads itself: the central's GAP Device Name (0x2A00), read once
per connection, 1.5 s after connect, with NimBLE's GATT client
(`CONFIG_MUSE_GADGET_BLE_PEER_NAME`, on; it selects NimBLE's central role
and GATT client): the worker starts the read and its answer comes on the
host task, so nothing waits; it's dropped if it isn't text. While the read
is pending, or after it failed (an ATT error, nothing there, not text, never
started), the row says "Connected: phone", and each failed read is counted:
`mg_ble_peer_name_failures()`, and `peer_name_fails=N` on the console's
`status` line (a number, never the name). `client_name` (18) is reserved
(the apps don't send a device name): not
listed, and a write or read of it is `unsupported` like any unlisted
setting. `client_app` (19: the app's name, then its version, each a length
byte, 0-`MG_CLIENT_APP_FIELD_MAX`, then UTF-8) is the client's, per
connection: empty at connect, cleared at disconnect, never stored; listed in
request_status's typed settings list and answered by get_settings as
written (a reply that would outgrow the outbox's 64-byte entry goes in
pieces, each a get_settings answer; a value the link's ATT payload can't
carry is `invalid_length`). A value over its limit, or one that isn't
well-formed UTF-8 text (overlong forms, surrogates, past U+10FFFF, control
characters), is refused whole with `invalid_value`; one cut short with
`invalid_length`; settings before it in the write still apply
(`mg_proto_utf8_text_ok`). A tap on the row shows it. The names are the
user's (mgcommands.h): no log line or console answer the Nordic
UART mirror can carry (the app relays it into its logs) prints `client_app`
or the GAP name, only whether one came and its length; the screen shows
them. `mg_ble_peer()` hands both to the page
(`components/muse/muse_status_conn.c` has the row's rules).
`tools/mg_ble_client.py` names its app after request_status where 19 is
listed: `mg_ble_client <commit>` (`--no-identity` leaves it unset; no
device name). `test_mg_ble` (`test_client_settings`),
`test_muse_status_conn` and `test_mg_ble_client`
(`test_identity_at_connect`) run them.

## The face (assistant_state)

A board with a display or status light (`mg_ble_platform_t.assistant_state`:
the boards with the full UI and the Voice PE) lists `mg_command_assistant_state` and runs
the Wi-Fi path's sequence for a BLE turn: listening while push-to-talk
captures, thinking once it ends (the client's reply is coming), then what
the client says: responding shows speaking, done the happy animation then
idle, error the error face for 3 s then idle, idle cancels. While a client's
audio plays (stream_audio) the face shows speaking, with the mouth following
the audio, and goes back to the last state after; a state that arrives
during playback applies once it ends. Thinking or responding with no update
for 120 s (tool calls can keep a turn quiet that long), or a disconnect,
returns to idle. An utterance saved to the
offline queue ends happy, since nobody is answering, and a capture the
client drove with `start_mic` returns to the face it had (the client sends
thinking if a turn follows). Boards with the full UI apply this in the voice loop
(`components/muse/muse_voice.c`), the Voice PE on its LED ring
(`main/voice.c`). Every face change of these turns is logged as
`mg.face: face <mode>` (`ring <state>` on the Voice PE), which the Nordic
UART mirror carries: `tools/mg_ble_client.py ptt` and `state` print them.

## Live call

The client is the Muse app, which implements the call on its own session; the SDK's reference apps don't.

`CONFIG_MUSE_GADGET_BLE_LIVE_CALL` (on by default on boards with the full
UI) offers `mg_command_live_call` (0x85) and lists it in supported_features:
the client (the Muse app) holds a live voice call with the Muse, and
the gadget starts it, ends it and shows it.

- **Starting:** Live call in Settings (touch boards) or the menu sends
  `[live_call, start]` and the screen goes black with only "Calling…"
  centered ("Waiting for phone…" with no client) until the
  call's first picture is drawn, which replaces it directly. A start no client has answered stays pending:
  it goes again after each request_status, and a disconnect keeps it. Any
  state but idle answers it (idle is what a client may say before it has
  seen the start); with no answer in 30 s the gadget drops it and says
  "No answer".
- **States:** connecting keeps "Calling…"; live keeps it until the first
  picture (on a build without video the face takes over at once, with a
  green "Live m:ss" pill under the state line; a phone that sends no picture
  for 15 s gets the same); ended and failed close it with banners
  ("Call ended" 1.5 s; "Couldn't start the call", "Call dropped", "No
  answer" 3 s), then the home. The connecting backstop:
  connecting for 60 s with no newer state from the
  client ends the call from the gadget: `[live_call, end]` as for the
  user's end, "Couldn't start the call" for 3 s, then the home; every
  connecting the client sends (a re-sent one too) restarts the 60 s
  (`muse_call_backstop.h`, `test_muse_call_backstop`). For the link policy
  Calling... and connecting are activity for at most 30 s
  from the start or the client's last connecting, live for as long as it's
  live (`mg_linkpol_call_busy`, `test_mg_linkpol`). A failed state may carry why (the
  live call features line `10 FE 85 02` lists
  failure_reason; `[85 04 reason]`): setup_required (1) shows "Open the Muse
  app to finish setting up calls" for 5 s instead (the phone needs the user
  first: the Muse app's voice intro, the microphone permission); the banner
  wraps inside 70 % of the panel. A client may start a call itself: connecting or live
  shows it the same way. A client that disconnects ends its call (the board
  hears idle).
- **Controls:** during a call, on screens of 320 px or more, a panel near
  the bottom holds volume − and + buttons around a level bar and a red End
  call button. It shows for 3 s at a time and comes back on a tap (the
  first tap only brings it up, and a tap on the top half hides it at once;
  never by itself); the first tap once the call shows its picture adds
  the hint "Hold to talk • tap for controls" for 5 s (the calling screen
  has no hint). It's one LVGL canvas: shown
  as itself on the face, and blended into each video frame while video
  plays (`muse_video_set_overlay()`; LVGL is paused then, so the level bar
  is drawn at the current volume each frame and the touch controller is
  read directly, every 10 ms on the input task).
- **Ending:** End call, Live call again, or `>call=0` on the console sends
  `[live_call, end]` and clears the call at once.
- **Volume:** − / + step a sixteenth; so does a vertical swipe anywhere,
  face or video (up louder, one step for a short swipe of 30 px and another
  for each 60 px further). The board's other buttons are volume keys in a
  call: on the StopWatch the top-left button (speaker) is up and the red
  bottom-left one down (a hold still powers off). Levels are kept like any
  volume change (the phone's `speaker_volume` reads them); down to 0 turns
  the speaker off. A call that starts (connecting or live) with the speaker
  at 0 or off raises it to 50%.
- **In a call:** push-to-talk, assistant_state and stream_audio work as
  outside one, and the face doesn't slide off to settings. Video the client
  streams (stream_video start_streaming) shows full screen without opening
  Phone camera, as any client-started stream does, and no button closes it
  (talk is push-to-talk, the others are the volume). When the call ends the
  view closes (stop_streaming, then request_streaming 0), from the input
  task.
- **Bench:** the console's `>call` / `>call=0` / `>call?` start, end and
  report (`@call <none|asked|connecting|live> <secs> volume <0-100>`); on a
  bench build (`MUSE_BENCH=1`) `>drag=233,350,233,230,300` is a swipe up
  and `>tap=X,Y` a tap (on the face; video's taps are read from the
  controller itself); call changes are
  logged as `mg.call:` on the Nordic UART mirror. `tools/mg_ble_client.py
  call` plays the phone: it answers a start with connecting then live
  (`--answer failed|none`), an end with ended, and can start a call itself
  (`--start`), send states (`--state`) or end after a while (`--end-after`).
  The state machine is `components/muse/muse_call.c`; the protocol side is
  `mg_proto_live_call_request()` and `cmd_live_call()` in `mg_proto.c`.

## Audio playback

A board with a speaker lists `mg_command_stream_audio` (29) and plays what
the client streams, as the protocol's Audio playback section says. The Muse
boards have one (`speaker_write` in `main/mg_glue.c`, through
`muse_audio_write_chained`, so the speaker volume and mute settings apply); the
Voice PE doesn't offer it yet.

- **Speech chain:** a board whose small speaker needs speech made loud for it
  (`muse_board_t.speaker_speech_chain`: the ESP-Mosaico, whose volume tops
  at DAC -5 dB, its amp's clip) hands the player `speaker_chain` and
  `speaker_chain_reset` (`components/muse/muse_spk_dsp.c`: 700 Hz
  4th-order high-pass, compressor -30 dBFS 4:1 3/120 ms +20 dB, -1 dBFS
  look-ahead limiter and clamp; the console's `>spk.dsp`), and the player
  runs it after the boost on every reply chunk, push-to-talk and Live call
  alike (`mg_chain.h`): silence goes through it while it still holds audio,
  so a reply's last 2 ms play; a barge-in pause leaves it alone (the rewind
  `speaker_clear` hands back is post-chain and is written as it is, and the
  chain goes on from the next sample: every sample once, no gain jump); a
  hold's claim or a flush in the pause empties it unplayed; a new stream
  starts it afresh. `test_muse_spk_dsp` runs the pause through it on the
  real `muse_audio_tx` core.

- **Boost:** a board with a quiet speaker sets
  `muse_board_t.speaker_boost_db10` (the StopWatch: +6.0 dB). Above half
  volume the decoded stream gets makeup gain, rising to that at 100, then a
  peak limiter (`mg_boost_t` in `mg_play.c`: instant attack, ~64 ms release)
  holds it under -4 dBFS, so a codec that adds a few dB of its own at the
  top (the StopWatch's ES8311: +3.6 dB) doesn't clip. The stream's `mg.play`
  level line is measured after it, and a `stream boost: +N dB, limiter on
  N ms` line follows it.

- **Capabilities:** SBC (mono; 16, 32, 44.1 or 48 kHz), LC3 (16 kHz, 10 ms,
  20-400 byte frames; only where `CONFIG_MUSE_GADGET_BLE_LC3` builds the
  decoder) and PCM (s16le mono, 8 or 16 kHz: faster PCM is over the
  256 kb/s maximum). Sample rates 16000 (the default), 8000, 32000, 44100
  and 48000; one channel. Rates other than 16 kHz are resampled to the
  speaker's 16 kHz by `mg_resample.c` (Blackman-windowed sinc, cut at 92%
  of 8 kHz, over 70 dB of alias rejection in the tests). The capabilities
  aren't per codec, so a combination a codec can't do (LC3 at 48 kHz, SBC at
  8 kHz) gets `invalid_value` at start_streaming. The list also carries
  flush (6 = 1, below) and live depth (7, uint16: the smallest depth in ms
  the gadget holds, `mg_play_live_floor_us()`: the 30 ms start, the
  decoder's 10 ms chunk and the board's I2S DMA depth, 70 ms on every board
  with musegadgets playback). start_streaming takes parameter 6, the live
  depth (uint16 ms; 0 or more than 1000 gets `invalid_value`). Its entries
  carry no length (mgcommands.h), so an unknown id ends
  the list: what came before it applies, defaults for the rest (an unknown
  first id starts the device's first codec at 16 kHz), never `unsupported`.
- **Buffer and flow control:** a `CONFIG_MUSE_GADGET_BLE_PLAY_BUFFER` ring
  (16 KB: 2 s of 60 kb/s SBC, 500 ms at the maximum bitrate), allocated at
  start_streaming and freed after the stream, in PSRAM when there is some.
  buffer_update reports its free bytes, the total received and queued ms
  (A/V sync: the audio held ahead of the speaker, `mg_play_queued_ms()`: the
  ring, what the playback task has popped but not yet written, and the I2S
  DMA's depth from the board's `i2s_dma_desc` x `i2s_dma_frames` at 16 kHz;
  0 with no stream, 0xffff when the board can't say), in reply to
  start_streaming and request_buffer_update, and from the worker every
  10 ms of playout when it changed, at least every 250 ms, for every reply
  stream (below). **Live depth** (mgcommands.h parameter 6 and capability 7):
  the cap is the client's
  choice. A stream started without parameter 6 gets the whole ring as
  credit, in a Live call or not, so an app that doesn't pace live never
  underruns against a cap it didn't ask for (a 100 ms call cap
  underran ~200 blocks a reply against apps that don't pace). With it the device
  holds its queued ms at max(asked, the floor) (`mg_play_t.live_us`; asked
  below the floor holds the floor), and audio in flight doesn't count
  against that (on one link ~70 ms was in flight, and a cap on queued + in
  flight left ~20 ms on the device and underran ~180 blocks a reply). The
  credit (`mg_play_credit`) is the depth less queued ms, plus what the device
  sees in flight, plus the in-flight allowance, at the stream's byte rate
  (LC3 at 32 kb/s with 30 ms of DMA, nothing held and nothing measured:
  70 ms, 280 B, in whole frames; SBC from its bitrate, else what its writes
  brought), 0 when that comes to nothing.
  - *In flight* (`mg_proto_t.flight_us`): the time from a buffer_update
    that raised the client's allowed total (received + credit) to the first
    byte past the total before it (bytes only that update allowed), smoothed
    over 8, at most 250 ms (a longer one is a client with nothing to send);
    kept for the connection. The device can't see what the client has
    written, and a credit counted against the whole allowed total grants
    each credit again before the client can act on it (on the 15 ms link
    below the depth swung 40-180 ms).
  - *The allowance* (`mg_play_t.live_allow_us`, for the jitter the
    measurement averages away): 0 to start, 20 ms more per underrun up to
    40 ms, 5 ms less per 5 s of audio played without one (the silence
    between replies doesn't count); kept across streams, forgotten on a new
    connection. Logged `mg.play: live depth 100 ms asked, holding 100 ms,
    in-flight allowance 0 ms` at the stream's start and `live depth:
    in-flight allowance 20 ms (underrun)` / `(clean)` as it moves.

  A changed credit goes out every 10 ms of playout (the worker looks every
  5 ms while a stream plays), once the previous buffer_update has left the
  stack (NimBLE's NOTIFY_TX on Control, `mg_proto_control_sent`; after 40 ms
  regardless): at most one per connection event, 12 B each. (On the Watcher,
  an app pausing at queued 100 ms underran 270 times in 70 s on the
  100 ms cadence.) Writes the client sent past the depth are kept, just not
  credited again until the depth falls. A stream that starts before the
  link is active has that cadence too, chat
  and push-to-talk replies alike, until active is granted: a pending
  notification keeps the peripheral listening at every connection event
  despite its slave latency, so a reply on a ready link (latency 5) arrives
  at every event rather than in bursts every sixth. **The whole-buffer path**
  (no parameter 6, link active; in a bench test of a reply with video, fixed 15 ms audio updates held the notify slot
  and made video credit late 5x as often, with no audio gain) updates on
  change instead: once 1 KB of room was freed since the last
  (`MG_PLAY_UPDATE_FREED`), or 50 ms on with any change
  (`MG_PLAY_UPDATE_CHANGE_MS`), never while any Control notification is
  still in the stack, 250 ms keepalive; the worker runs video credit before
  it, so when both are due video takes the one in-flight slot. `test_mg_ble`
  runs a whole-buffer reply beside a 300 kb/s video with 3 KB in flight (the
  vcredit sim): no video credit stall over 40 ms, no longer than video
  alone, and the reply plays with 0 underruns.
  `test_mg_ble` runs a client that writes 20 ms only when credit allows
  over a 15 ms link with 0-30 ms of delivery jitter: without parameter 6,
  in a call or not, it fills the ring; with a live depth of 100 ms, 60 s:
  0 underruns, queued settling at ~88 ms (50-120), ~40 ms measured in
  flight, the allowance untouched, one update per connection event; two
  forced 150 ms outages each underrun once, the allowance grows to 40 ms
  and is back at 0 by the end with no other underrun; with 50 ms more of
  stack delay (~90 ms measured in flight) one underrun as it learns and
  ~88 ms held (counted against the depth instead: 109 underruns, ~53 ms
  held). Data writes go into
  the ring straight from the NimBLE host task, so none is lost to a full
  queue or overtakes the Control command before it. A write that isn't
  whole frames of the stream's format, or doesn't fit, is dropped whole (so
  the frames after it still line up) and counted as received and dropped.
- **Playing (live reply audio, at most ~100 ms of depth):** the
  `mg_play` task (priority 7, above the voice loop) pops about 10 ms of
  whole frames, decodes and resamples them outside the lock, and writes
  10 ms chunks to the speaker, whose I2S DMA paces it. It starts once it
  holds 30 ms (`MG_PLAY_PREBUFFER_US`, at most 40), or 150 ms on a link that
  isn't active (below), and never re-buffers: a
  gap (an underrun, counted once) is concealed chunk by chunk
  (`mg_dec_conceal`: LC3's packet loss concealment, liblc3's or lc3fx's
  `lc3_decode()` with no data, for the gap's first 60 ms; SBC and PCM the
  last 10 ms once more, faded out; then silence), and the next frame that
  arrives plays at the next chunk. An underrun is counted when audio comes
  back within 400 ms of the gap's start (`MG_PLAY_IDLE_US`), at that start on
  the stream's clock, its length added to `gap_us`; a longer gap is a reply
  boundary (`idle_gaps`), not an underrun. Outside a Live call it ends a
  stream the client went quiet on without stop_streaming after 10 s
  (stop_streaming drop).
- **A reply that starts on a link that isn't active** (on the Watcher,
  a chat reply on a ready link, active
  granted ~1 s later, played with 6 underruns at the 30 ms start): it starts
  at 150 ms held (`MG_PLAY_SLOW_START_US`), or at its end of data when the
  whole clip is shorter (stop_streaming keep, or no data for 100 ms with
  less held, `MG_PLAY_SLOW_END_US`); once active is granted the normal
  30 ms applies, to a stream still waiting too. mg_ble.c sets
  `mg_play_t.slow_link` from the link in effect (`mg_linkpol_link_active`:
  15 ms, latency 0) at every policy round, connect and connection update.
  start_streaming asks for active before its first buffer_update
  (`ops.stream_starting`: the request ahead of the answer). A Live call
  holds active for the whole call, and its stream (`keep_open`) keeps the
  normal start whatever the link. `test_mg_ble` replays the trace on a
  modeled link (connection events, slave latency skipped unless a
  notification is pending, three writes an event): the 7.7 KB reply written
  in 65 ms on a 30 ms / latency 5 link, active granted after 0.9 s, plays
  with 0 underruns (a fixed 30 ms start underruns on the same model); and the
  Watcher's timeline on the policy (press, 2 s of capture, 8 s thinking, the
  reply): active throughout, the reply at the normal start, 0 underruns,
  ready 5 s after it played.
- **Adaptive start** (`mg_play_t.start_us`,
  `mg_play.h`): per connection, a reply outside a Live call starts at
  30 ms held at connect. One that underran in its first second of audio
  (`MG_PLAY_EARLY_US`, from its first audio; `st.early_underruns`) makes the
  next one start 20 ms higher (`MG_PLAY_START_STEP_UP_US`), up to the live
  depth's floor (`mg_play_live_floor_us`, the listed 70 ms) at most; each
  clean reply (no underrun at all) eases it back 10 ms
  (`MG_PLAY_START_STEP_DOWN_US`), never below 30 ms; a reply with only later
  underruns leaves it. It's judged when the next stream starts, on the one
  before if that played audio and wasn't a call's; `mg_play_live_forget()`
  (a new connection) resets it, and the last link's reply isn't judged. A
  Live call's stream keeps 30 ms and the slow link's 150 ms start is
  unchanged. Each move is logged at the new stream's start, `mg.play: start
  threshold 50 ms (early underrun last reply)` (`clean reply` going down);
  early underruns still count as underruns, and the `av play end` line adds
  `early=N`, so app-side gaps still show on the bench. `test_mg_play`: an
  underrun in a reply's first second raises the next start 20 ms, clean
  replies lower it, it stops at the floor, a later underrun leaves it, a
  call's stream is neither judged nor raised, and a new connection resets
  it.
- **One reply stream per Live call** (mgcommands.h "Live reply audio"): the app opens stream_audio once (at live or the first
  reply) and keeps it open to the call's end, writing nothing between
  replies. A stream started while the call is connecting or live is kept
  open (`mg_play_t.keep_open`, also set and cleared as the call's state
  changes): silence after the concealment fade however long it runs empty,
  the next reply's first frame at the next chunk (10 ms, then the 30 ms
  DMA), no re-buffering. It ends on stop_streaming or a disconnect: the
  device ends a call's stream only when it can't go on (mgcommands.h). **The device's own sound in a call** (a chirp, the Sound
  page's test, the MP3 or loopback test, a saved note going out: whatever
  takes the speaker through `mg_proto_claim_audio`) pauses the call's
  stream and resumes it after: what it holds stays
  (no flush, nothing lost), Data and credits go on as usual (the call's
  live depth, where asked, bounds what piles up), a start_streaming meanwhile is taken and
  waits the same way (`mg_proto_play_held`), and the playback task logs
  `paused for the device's sound (N ms kept)` / `resumed after the device's
  sound`. It pauses rather than ducks: no ESP32 board mixes two sources in
  its output path (the reply's player and the voice loop each write the
  speaker). Outside a call the sound still ends a reply (stop_streaming
  drop) and refuses a start as busy. The call's end only clears keep_open, so a stream the client
  leaves starved after the call ends 10 s later as any other. A talk
  press barges in by flushing it, never ending it (`mg_proto_claim_turn`,
  the same as a flush below, with its buffer_update): the app keeps the
  stream and never reopens it, so ending it would lose every later reply;
  the next reply's audio, written during the turn, waits for the turn and
  its tail and then plays. Outside a call a press still ends the stream
  (stop_streaming drop). A mid-call mute is the app's (it stops writing and
  flushes; the gadget's speaker_volume report starts it). "Playing" for the speaking face, the panel hold and
  "playback over" is audio playing (`mg_play_audible` / `mg_voice_audible`:
  the stream holds some, or played decoded audio within 300 ms of its
  clock), not the stream open (`mg_voice_playing`, which keeps the codecs
  from resting so the next reply starts at once). The `av play` line runs
  while the stream is open. An empty ring between replies grants the whole
  ring, or with a live depth the depth less the DMA's 30 ms (plus what's
  measured in flight and the allowance).
- **Flush (`mg_stream_audio_flush` = 6, capability 6 = 1, listed):**
  barge-in. Handled where the Control write is (the NimBLE host task, like
  Data), so in order with the Data writes around it: Data before it goes,
  Data after it plays. `mg_play_flush` empties the ring and what the task
  holds at once; the playback task, seeing `flush_seq` move, fades its
  FIFO's first 5 ms out (no click), drops the rest and restarts its decoder
  and resampler (`mg_dec_flush`: nothing of the flushed reply is
  concealed; the stream's stats stay). The stream stays open with its
  counters running, the gap after it is neither concealed nor an underrun,
  and the device answers buffer_update with queued ms 0. Logged `mg.play:
  flush (N ms discarded)`; counted in `flushes` / `flushed_us`.
- The output path
  is 30 ms (3 x 160 frames of I2S DMA, `MUSE_AUDIO_DMA_LIVE_*` in
  `muse_audio_dma.h`) on every board with playback: the StopWatch, the
  SenseCAP Watcher, the ESP-Mosaico, the Waveshare S3 1.75C and the C6 1.8.
  Each board makes its duplex channels so; the Waveshare ones without their
  BSP's `bsp_audio_init()`, which takes no channel config (ESP-IDF's
  6 x 240, 90 ms), setting the codecs up as the BSP does. With the client's
  60 ms start burst and 40 ms writes the whole depth stays at or under
  100 ms (`test_mg_play`). Each second's `av play` line says `queued=MS`
  (-1: unknown); the end line adds `dropped=N repeated=N` (the drift trim,
  below). stop_streaming keep plays out the buffer and
  then notifies keep; drop empties it and notifies drop at once. A
  start_streaming over a stream still playing (or playing out) notifies that
  stream's stop first, then the new buffer_update: every client stop is
  answered once. A client's video stop_streaming is never answered; the
  device notifies a video stop only when it ends the stream itself.
- **Drift trim (`mg_trim_*` in `mg_play.h`):** the gadget's
  output clock may run off the reply's rate, so the depth creeps up (a slow
  output) or down (a fast one) over a long reply. Once a chunk the task
  looks at the depth it holds ahead of the DMA (the ring + its FIFO,
  buffered_us + pending_us), averaged over 100 ms windows and smoothed over
  about 400 ms (the client's packets make a sawtooth; the link's jitter
  moves it).
  - **Target:** the stream's own steady depth, measured over the 2 s of
    playing after its first second, and beside it the floor (the lowest the
    depth went meanwhile: the troughs). No constant fits every client: a
    60 ms burst in 20 ms packets with up to 15 ms of jitter holds about
    37 ms, with the troughs 25 ms under it at worst. Logged `trim target
    67 ms` (as queued ms, DMA included).
  - **Start:** more than 40 ms above the target for 2 s drops one sample in
    1000 (0.1 %); more than a third of the floor (40 ms at most) below it,
    the window not starved, for 2 s, repeats one in 1000. The band below is
    narrower because the troughs run dry before the mean moves 40 ms. A gap
    of 100 ms or more starts the count over. Logged `trim -0.1% (depth
    130 ms)` / `trim +0.1% (depth 52 ms)`.
  - **Rate:** every 500 ms a running trim looks again. Still past its band
    and not gaining (by 0.25 ms) on the target: twice the rate, up to one in
    250 (0.4 %). A faster rate that has reached the target: half the rate;
    0.1 % within 10 ms of it (half the band below, when that's less): off
    (`trim off (depth 99 ms)`). Slowing or stopping waits 2 s after the last
    slowing, so a clock between two rates holds there instead of stopping
    and drifting past the band again.
  - **How:** on the decoded 16 kHz PCM before the speaker's gain, a trim
    resamples 33 samples to 32 (a drop) or 32 to 33 (a repeat) by linear
    interpolation: the waveform stays continuous (no step, so no click), a
    3 % pitch change for 2 ms. A repeat that has no room in the FIFO, or a
    block too short for the 32, waits for the next block.
  - **With a live depth** the credit holds queued ms at the depth plus the
    in-flight allowance (above), so the steady depth moves with the
    allowance: the target follows it (the allowance's change since the
    target was measured is added to it) and never sits above the depth plus
    the allowance, less the DMA (`mg_trim_t.live_cap_us` and
    `live_allow_us`, set by the player before each look; `mg_trim_target()`).
    A depth past that by the band drops back to it; a slow output's surplus
    otherwise waits at the app. The repeat side guards a fast output as
    before.
  - **Repaying gaps:** with the depth
    capped (a live depth), a concealed gap adds its length to the reply's delay
    for good (the audio waits on the phone). Each underrun's gap
    (`gap_us`) is owed (`mg_trim_owe`), and while it is and the stream
    holds 20 ms or more the same drop runs at one sample in 240 (~0.4 %),
    the drift trim's own rate waiting, until it's made up. A reply
    boundary (a gap of 400 ms or more) or a flush clears it
    (`mg_trim_forgive`). Logged `repay 80 ms` / `repaid`. `test_mg_play`:
    an 80 ms gap at 20 s and 40 s on a 40 ms link over 60 s: the delay is
    back before the next gap, never drifts up, and repaying adds no step
    past the tone's own; without it each gap stays (+175 ms by the end). The
    run has a live depth of 100 ms with nothing in flight, so each gap also
    grows the allowance by 20 ms, which the target follows.
  - **Counted:** the stream's dropped and repeated samples and the gaps
    repaid on its `av play end` line (`dropped=N repeated=N repaid=N`).
  - **Tested** (`test_mg_play`): the decisions on a fake clock, the
    interpolated drop and repeat on a tone (no step steeper than the tone's
    own), and 60 s replies through the player's loop into a 3 x 160 DMA
    whose clock is 0.1 or 0.3 % slow or fast, the client's 20 ms packets
    with up to 15 ms of jitter: every second's mean depth within 50 ms of
    the target, no click; a correct clock never trims. A 0.3 % fast output
    eats the troughs' headroom in about 3 s, before the 2 s hold and the
    climb from 0.1 % catch up: one gap is concealed then, none after.
- **Clock:** boards with the full UI take `device_action set_unix_time`
  (18) and answer `get_unix_time` (19) with `report_unix_time` (20)
  (`mgcommands.h`, Clock), through `components/muse/muse_clock.c`: the time
  goes into the system clock (kept across disconnects and software
  restarts), the UTC offset and 12/24-hour flag into NVS when they change,
  for a clock on the face (`muse_clock_local()`, `muse_clock_24h()`). On a
  set-up gadget an unproven central's offset and flag apply in RAM only;
  NVS takes them from a proven one (Token proof, below). A time before
  2024-01-01T00:00:00Z (`MG_CLOCK_FLOOR_MS`) is refused with `invalid_value`
  and the clock keeps its time. Other builds answer unsupported. The typed list
  `[supported_features, sub_feature, device_action, ...]` names every action
  the build implements: `throughput_test` (11)
  always, 18/19/20 with a clock, 8/9 and 10 below.
- **Battery and power:** a board with `read_power` answers
  `request_battery_status` (8) with `report_battery_status` (9: mV, current,
  SoC, °C, remaining and full capacity, the reading's device ms, flags), and
  sends 9 unsolicited when charging or USB power starts or stops. It comes
  from the input task's latest reading (`muse_battery_reading_us()`), never a
  bus access from the BLE worker: busy before the first. What each board
  measures: the ESP-Mosaico's BQ27220 everything (current measured, the
  cell's temperature, 80 mAh capacities); the StopWatch's M5PM1, the Watcher's
  ADC and the C6's AXP2101 the voltage, a percentage and charging (current,
  temperature and capacity 0, flags say so). A board with `power_off` takes
  `power_off` (10, no reply) through the same goodbye as its Power off; the
  ESP-Mosaico lists it only on battery (`power_off_battery_only`: USB holds
  its latch on). Unknown commands and actions get
  `[error, cmd, code, unsupported]`, never silence.
- **What a board can draw:** Live call (0x85) and subtitles (0x87)
  draw into PSRAM buffers, so a board without PSRAM (the Waveshare C6) neither
  lists nor accepts them (`board_draws_calls()` in `main/mg_glue.c`;
  `mg_ble_live_call_offered()` tells the UI), and subtitles drop out of the
  list while their band's buffers aren't allocated
  (`mg_ble_platform_t.subtitle_ready`). Video (0x84) is listed only with a
  `draw_frame` panel and its ring allocated.
- **A phone's indications:** the Muse boards build NimBLE without the
  central role, so without its GATT client, which leaves an incoming Handle
  Value Indication (a phone's Service Changed right after connecting) with
  no handler: NimBLE answered it with an Error Response for opcode 0x1D,
  and Android's GATT client took that for the answer to its first service
  discovery (0 services, the gadget unusable). `mg_att_confirm.c` wraps
  NimBLE's error reply and sends the Handle Value Confirmation instead.
- **Push-to-talk wins:** the voice loop claims the audio path for its own
  turns, chirps and bench tests (`mg_voice_claim`): a stream in progress
  ends with stop_streaming drop, and start_streaming gets `busy` until the
  loop is idle. Disconnecting ends a stream silently. While a client's
  audio plays, the voice loop keeps the codecs powered and keeps it out of
  the push-to-talk pre-roll. The reply goes quiet at the talk press's touch-down
  (`mg_proto_barge_pause`), not at the hold threshold, so the ~250 ms of
  pre-roll recorded before the turn is claimed holds none of the Muse's own
  words: the playback task holds it as for a turn (ring and decoded FIFO
  kept, Data and credits going on, no underrun counted; a client's stop or
  flush still applies), a tap resumes it from the sample it paused at
  (`mg.play: paused at the talk press (N ms kept)` / `resumed after the talk
  press`), and the hold threshold claims the turn (stop_streaming drop, or the call's flush); a pause nobody
  ends lapses after 1.5 s (`test_mg_ble`'s barge pause, `test_muse_ptt_ui`'s
  touch-down cases). The touch-down also silences what the speaker's I2S
  DMA already held (~30 ms, which played into the pre-roll: +5.1 dB over
  the room in a barge-in's first 100 ms at volume 30): the playback
  task calls `speaker_clear` (`muse_audio_speaker_clear`,
  `components/muse/muse_audio_tx.h`), which fades the DMA's buffers to
  zero in place 2 ms past the read position over 4 ms (no click; the I2S
  bus, which clocks the mic in full duplex, never stops) and hands back the
  samples that were there unplayed; a tap's resume writes them first,
  ramped in over 5 ms (`mg_play_ramp_in`), so no sample is lost,
  and a hold's claim, a client's flush or drop
  discards them (`paused at the talk press (N ms kept, M ms back from the
  speaker)`; `test_muse_audio_tx` against a model of ESP-IDF's TX driver,
  `test_muse_spk_dsp`'s barge through the chain, `test_mg_ble`'s barge DMA clear). Every board
  with a speaker attaches its TX channel
  (`muse_audio_attach_tx`, before `i2s_channel_enable`).
- **Speaker volume:** `mg_setting_parameter_speaker_volume` (10) is the
  Muse speaker setting, through `main/mg_glue.c`: get returns the volume, or
  0 while the speaker is off; set 0 turns it off (the volume is kept), any
  other value turns it on at that volume. The board persists it; a set to
  the level it already plays at changes nothing (nothing saved). The bench
  volume, the console's `vol=N` (USB `>` and `!IN`; bench and test mutes
  are RAM-only), sets the speaker
  in RAM only, never saved: `vol=saved` or a reboot puts the saved volume
  back, and a user's or a client's change ends it (the saved values back,
  then that change saved); `vol?` reads both (`@vol N saved M`).
  `mg_ble_client.py play --volume` and `av60.py --volume`
  set a run's volume this way and put it back. The gadget shows every
  change it didn't make: a client's set opens the volume overlay
  with "Set by phone" (a console's `vol=`: "Set by bench"), and muted shows
  in the top row and on the Sound page for as long as it lasts
  (`esp32/AGENTS.md`). A device change's report is owed until it leaves on
  Control: with no client ready, the stack refusing or replies queued ahead
  of it, it goes at the first pump that can send it (each connection's
  ready included) with the level then, and not at all if that's the level
  the client last saw (`mg_proto.c`, `spk_seen` / `spk_owed`). Boards
  without a speaker answer `unsupported`. With the speaker off, a stream is
  accepted and paced but plays silence.
- **Stats:** each stream ends with three `mg.play` log lines, mirrored on the
  Nordic UART service: frames in and decoded, bad frames, underruns and
  silence; bytes received and dropped, and milliseconds played; and the RMS
  and peak of the decoded audio and of what reached the speaker's codec
  (zero while the speaker is off, marked `speaker off`).
- **Loopback:** the ES8311 runs capture and playback at once (the same I2S
  bus, as `muse_audio_loopback_test` does), and a client's start_mic isn't a
  claim, so a client can record the mic while its own audio plays:
  `tools/mg_ble_client.py play --loopback` checks the tone comes back.

## Video

With `CONFIG_MUSE_GADGET_BLE_VIDEO` (the StopWatch's `stopwatch-video`
build: `tools/muse/board.sh build stopwatch-video`, overlay
`devices/sdkconfig.muse-video-ble`) a display board takes
`mg_command_stream_video` (`../../../protocols/mgcommands.h`, Video
streaming): H.264 constrained baseline from the client on Data, with the
same credits as audio playback. BLE is one transport of the shared video
core (`CONFIG_MUSE_VIDEO`, `../../AGENTS.md`, Video from a phone): the ring,
the frame format, decoding and drawing are the Wi-Fi path's;
`main/mg_glue.c` registers this one as a `muse_video_transport_t`.

- **Receive:** `mg_access` puts each Data write straight into a 64 KB ring in
  PSRAM (`components/muse_vstream`, `CONFIG_MUSE_GADGET_BLE_VIDEO_BUFFER`), on the host
  task. The decoder copies out one frame (`MG_VIDEO_FRAME_HEADER_LEN`-byte
  header, then the access unit) at a time; a bad header makes it search for
  the next one and flag that frame (`after_gap`). buffer_update (with frames
  shown, dropped and the average decode time) returns video credit promptly
  (from a bench test of a reply with video: the apps keep only 2-3 KB of video in
  flight during a reply): once 1 KB has been taken from the ring (the decode
  task tells the worker per frame taken, before decoding it) or 1 KB
  received, else 20 ms after any news (bytes in, room freed, a picture
  shown), never batched behind a decode or held to 100 ms; at most one in
  flight per connection event (the next waits for the last one's NOTIFY_TX
  on Control, `mg_proto_control_sent`, which wakes the worker, or 40 ms), at
  least every 250 ms for camera video, nothing while a UI stream is still;
  request_buffer_update is answered at once. `test_mg_ble`: a client keeping
  3 KB in flight until the decoder takes it, a 15 ms link, 0-30 ms of jitter,
  25 fps at 300 kb/s for 60 s, keeps the rate with no stall over 40 ms (the
  old eighth-of-the-ring / 100 ms rule fell 9.8 s behind).
- **Decode and show** (`components/muse/muse_video.c`): the optimized
  tinyH264 (`components/tinyh264`, hot code in IRAM with
  `CONFIG_MUSE_VIDEO_DECODER_IRAM`, which the overlay turns on) on core 0 reports rows
  as they become final, and the UI core draws them as they come: converted
  with the S3's SIMD (`muse_yuv_pie.S`), two times upscaled, straight into
  the panel's DMA buffers while the previous piece is on the wire
  (`muse_lcd_bands_frame`: one window per frame, then Memory Write
  Continue), with LVGL paused. On a
  round panel (`muse_board_t.round`: the Watcher, StopWatch and Waveshare
  S3 1.75C) the picture covers the circle: the window is the whole
  panel and the 2x picture is centered on it, cropped where it's larger
  (the corners the round mask hides) and its edge pixels extended where
  it's smaller, through a block of four 2x rows (`muse_video_fit.c`); the
  boards suggest 208 (Watcher, 2 px a side cropped) and 240 (466 px panels,
  7 a side). Two slots: picture N shows while N + 1
  decodes. On the ESP-Mosaico (S31) it decodes up to 480 px: two times
  upscaled up to 240, 3/2 at 320, one to one at 480, and with two bands
  (mg_stream_video_parameter_tiles) the bottom band decodes on the UI core
  at the same time (`../../../protocols/VIDEO.md`, Larger pictures). A frame the decoder can't use (lost bytes, a decode error) makes
  it wait for a keyframe and send request_keyframe (300 ms apart at most).
- **Link:** link modes (above): a stream is activity, so the link is
  active (12/12/0/400) while it runs (`>video.itvl=N[,ce]` on the console
  tunes the interval and event length). The overlay raises NimBLE's
  buffers (its ATT MTU is the BLE gadget overlays' 247); clients write at
  most 512 bytes (the ATT value limit).
  The stream may instead come on an LE L2CAP channel (capability
  `l2cap_psm`, 8 KB SDUs), measured slower than GATT on an S22.
- **Audio beside video:** audio playback (stream_audio) runs alongside a
  video stream. The client marks which stream its Data writes belong to with
  `change_data_type` on Control (video, then the playback codec, and back);
  `mg_access` applies it on the host task, in order with the Data writes, not
  on the worker like other commands. Each stream keeps its own credits.
  Starting a second stream leaves Data with the first until a
  `change_data_type`. When the client ends either (its stop_streaming, or a
  new start), Data goes to the other (Android and iOS rely on it). When the
  device ends one (its own stop_streaming notification: its own sound taking
  the speaker, a starved or failed stream, the video view closing), Data
  selected for it is discarded until the client's next `change_data_type`
  (mgcommands.h: the writes in flight were the ended stream's),
  counted in `rx_discarded`, and told once on `mg.ble` at that switch: `Data
  discarded after device end: N bytes until change_data_type`. A new start
  while it's held keeps it held if the other stream is on, else takes Data.
  A dropped or undecodable video frame asks for a keyframe at once, then
  every 300 ms while the decoder waits. The L2CAP channel carries video only.
- **Link:** active while a stream runs (link modes, above), and back to the
  app's mode only 5 s after the last stream ends, so a client that stops
  and restarts (a new clip or size) keeps the active link.
- **Playout delay:** a client sending a pre-recorded clip can ask for
  `mg_stream_video_parameter_playout_delay` (offered up to 500 ms in
  report_capabilities). The decoder then starts frame N at the first frame's
  arrival plus the delay plus N frame periods, by frame number (the panel
  draws while it decodes), so frames that arrive unevenly still show
  evenly; a late frame goes at once and moves the schedule later. Live
  sources leave it out and get each frame as soon as it's decoded. The
  stats line gives the largest gap between pictures and how many took over
  1.5 frame periods: clips from Muse Gadgets Test show none at 200 ms
  (max gap 62 ms); unpaced, BLE's unevenness shows 5-25 per 10 s, up to
  177 ms.
- **UI:** the menu's Phone camera item opens the view and sends
  request_streaming 1; any button closes it (stop_streaming, then
  request_streaming 0). A client may also start a stream on its own: the
  view opens, waking a dark screen (start_streaming is refused as busy until
  it's on; clients retry). The console's `>video` / `>video=0` do the same
  as the menu.
- **BLE only:** the overlay makes the board a BLE-only gadget
  (`CONFIG_MUSE_GADGET_BLE_STANDALONE`): with Wi-Fi up the StopWatch has
  too little internal RAM left to start the BT controller, and Wi-Fi
  coexistence would take BLE airtime. The panel stays at 40 MHz: 80 MHz
  through the GPIO matrix hung its SPI bus now and then.

Measured on a StopWatch with an S22 (Muse Gadgets Test, `android/musegadgetstest`),
real scene: 30 fps at 450-620 kb/s (adaptive), no frames dropped, decode
24-27 ms, 26 ms to draw a frame, 31 ms from a frame's last byte in to it
being on the panel; the phone's camera to an encoded frame takes ~82 ms.
GATT carries up to ~870 kb/s at 15 ms (the phone sends 3-6 packets per
connection event; 7.5 ms and 30 ms both carry less). Two-way audio fits:
video at 30 fps plus SBC playback (60 kb/s, interleaved in 40-64 ms writes,
~40 type switches a second) plus LC3 capture (33 kb/s up) ran 30 s with no
video frames dropped, every audio frame decoded and no capture frames lost;
core 0 ~78% busy, core 1 ~33%.

## UI streams

A UI stream (`mg_stream_video_parameter_content` = ui; `../../../protocols/UI.md`)
is the same stream at the panel's own size, frames only when the picture
changes. The board offers it as `mg_stream_video_capability_ui` (last in
`report_capabilities`: older clients stop at a capability they don't know):
the StopWatch takes 466x466, one to one, in one band or two (the top band a
whole number of macroblock rows, the crop in the bottom one; `tile_split`
60 is sent for it), frames of up to 128 KB (half the 256 KB ring of
`devices/sdkconfig.muse-video-ble`). What differs from camera video:

- `start_streaming` with content ui opens the view by itself (as any stream
  does), and refuses a playout delay. Like camera streams it answers busy
  while the screen is asleep (it wakes it): clients retry.
- Every frame is decoded and shown and the last stays on screen for as long
  as the stream is on, with no timeout; the screen doesn't auto-sleep. If it
  went dark anyway, the gadget asks for a keyframe when it's back.
- `buffer_update` echoes the last frame drawn and its age, and during a UI
  stream goes out only when something changed.
- The link (link modes, above): a still UI stream isn't activity once no
  Data write has come for a second and no touch for 6 s, and 5 s later the
  gadget goes back to the app's mode (ready, 24/36/5/400); the next Data
  write or touch makes it active again. `>ui.lat=N` (or the same line on the
  Nordic UART) changes ready's latency.
  (The measurements below were taken with the earlier latency 9 after 1 s.)
- One line per frame on `muse_video` (on the Nordic UART mirror too): kind,
  bytes, each band's decode time, the draw and received-to-shown, and free
  memory at keyframes. `>ui.md5=1` adds each decoded picture's MD5 (the crop,
  I420), to compare with the encoder's.

Measured on the StopWatch (S3, 466 px one to one), H.264 UI test streams
from a Mac (`tools/mg_ble_client.py ui`), every picture's MD5 equal to the
encoder's:

| frame | bytes | decode, one band | decode, two bands (top / bottom) | received to shown |
|---|---|---|---|---|
| keyframe, text page | 27.5 KB | 87 ms | 53 ms (53 / 51) | 87-110 ms |
| keyframe, web page | 20.7 KB | 79 ms | 57 ms (45 / 57) | 80-101 ms |
| refinement (P, QP 24 / 18) | 12-16 KB | 57-64 ms | 43-45 ms | 69-91 ms |
| clock tick (P) | 1.1-1.6 KB | 30-31 ms | 31-33 ms | 52-57 ms |

The draw (466x466 RGB565, 40 MHz QSPI, the colour conversion in C) takes
50-90 ms but runs while the decoder works, so a picture is on the panel
~25 ms after its decode ends. Free memory during a stream: 16-19 KB
internal (largest block 9-13 KB), 3.8 MB PSRAM. A Mac central moves only
~120 kb/s (an S22 several times that), so from the Mac a 1.3 KB change
showed in a median ~130 ms with the link never relaxed, ~160 ms at latency
4, ~200 ms at latency 9 and ~260 ms at latency 19: the first change after
a still spell waits for the gadget's next wake and the update back to
latency 0 (~210-430 ms on the Mac, ~280-840 ms on the S22). With nothing
changing the stream sends nothing at all. What the relaxed link saves is
the radio's connection events on the gadget's side: at a 15 ms interval it
listens 66.7 times a second at latency 0 and 6.7 times at latency 9 (a
tenth); the current itself wasn't measured (the StopWatch's PMIC reads only
the battery voltage, and a PPK2 measurement needs its fixture).

## UI input and HID

`mg_command_ui_input` on a board with a touchscreen (`mg_ble_platform_t.touch_w`):
the typed list says touch, and with `CONFIG_MUSE_GADGET_BLE_HID` the three
HID modes.

- **Touch events:** `touch_enable` per connection, from any central: touch
  events and their coordinates stay open, with or without the token proof,
  as on every Muse Gadget (`hid_map`, a kept setting, and `hid_key` still
  wait for the proof on a set-up gadget, in every build). The board feeds a sample
  every ~10 ms (`mg_ble_touch()`: from LVGL's own reads while it runs, from
  the input task while the video view has LVGL paused, `muse_touch.c`) and
  `mg_touch_feed()` makes down, move (every 16 ms at most, only when it
  moved) and up. While a UI stream is on, Muse's own screens get no touches.
  `>touch=X,Y[,X2,Y2,MS]` on the console taps or drags as a finger would.
- **HID over GATT:** the HID service (0x1812: HID Information, Report Map,
  HID Control Point, Protocol Mode, an input Report and Report Reference for
  each of reports 1-3, report 1's feature report with the contact count
  maximum) joins this component's services, last, so the others' handles
  don't move. Reports and the map need an encrypted link: NimBLE's security
  manager bonds with Just Works, both sides' identity and encryption keys
  (phones connect from resolvable private addresses; without the peer's IRK
  the bond isn't found after a restart and the phone drops it), bonds in
  NVS. The service stays hidden until a client's `hid_map` asks for a mode
  (macOS, and likely iOS, ask to pair with any HID device an app connects
  to), then stays shown for bonded hosts, remembered in NVS with the map:
  hiding it under a bonded host makes Android drop the input device, so mode
  off only stops the reports. To a phone without a bond it's hidden at each
  connection whatever is saved (an iPhone asked to pair on every connection
  once an earlier app had left HID on), and shows once that phone's own
  `hid_map` turns HID on. A bonded host whose address wasn't resolved at
  connect gets it at encryption. Shown, it adds 0x1812 and appearance 0x03C0 to the
  musegadgets advertisement. `hid_status` answers `hid_map`, then the
  service shows (Service Changed: an unbonded app rediscovers).
- **The mapping** (`mg_ui.c`): a panel pixel goes through the inverse warp
  (flag fisheye) into the square, onto the screen by the map, and out as a
  digitizer contact or an absolute pointer at 0..32767, or a relative
  pointer parked top-left with -127 moves and stepped to the point in at
  most 8 counts (the map's counts across the screen), then button 1.
- On the S22 (`android/musegadgetstest`'s UI tab, Pair): bonded, Android's
  HID host attaches the gadget as a touchscreen, injected taps land on the
  pixel the formulas give, and after a gadget restart the S22 reconnects
  by itself, resolved to its identity, subscriptions restored. The gadget
  then shows "app connected": NimBLE restores mg's subscriptions for a
  bonded phone too, app or no app. With one connection, the OS's link
  takes it: an app finds a bonded gadget by its address, not by scanning.
  Android makes the whole HID device one touchscreen (hid-multitouch claims
  it), so the two mouse reports do nothing there: the digitizer is
  Android's mode, the relative pointer iOS's.

## Firmware update (MCUmgr SMP)

`CONFIG_MUSE_GADGET_BLE_SMP` serves firmware updates over Bluetooth (the
Muse apps, or a web updater): the
MCUmgr SMP core in `smp/` (vendored, don't edit it here) under
`mg_smp_esp.c`. The backend holds the app's first 352 bytes
(`MUSE_IMAGE_HEAD_BYTES`) in its scratch and writes nothing until
`muse_image_check()` (`../muse_image_id`) says the image is this board's:
the header's chip id, then the board description's product code and board
profile, and BLE updates kept (the BLE recovery tier). Anything else,
including an image built before the description, is refused with
`MG_SMP_EINVAL` (rc 3, the core's "wrong board in the payload") and logged
on `mg.smp` (`update refused before writing: ...`); the other slot keeps
what it held, since `esp_ota_begin` with sequential writes erases nothing.
Past the check, the core's SHA-256 checks and `esp_ota_end`'s image and
RSA checks run as before. AGENTS.md (Firmware update over BLE) has the rest.

**Who may update** (`protocols/mgcommands.h`, Token proof;
`test-vectors/mg-update-v2.json`): `mg_smp_gate.h` decides from the
connection (`mg_ble.c` `smp_gate_now()`), and on a link that needs the press
the request front (`mg_smp_front.c`) asks about each upload, image state
write, reset or erase whole before the core sees it: the vendored core asks
its `may_change`/`may_upload` hooks only at a transfer's start, so the front
reassembles every request, keeps the slot's running SHA-256, answers rc 10 or
11 itself, and the hooks say yes only to what it let through. No phone, token
or pre-armed hold is needed. A connection whose token
proof matched (the Muse apps), the wired console's `update.arm`, a SAFE the
user entered with the key, and bench, automation and test builds take it at
once, never waiting for a person. Any other link (a web updater, set up or
not) gets ONE confirm press: the first change starts "Press to confirm
update" on the gadget (the update card, a touch board's Confirm, or the status
light's confirm pattern on a screenless board) for 30 s and is answered rc 10
(`MG_SMP_EBUSY`, the core's `refusal_rc`) while it asks; the client retries
about once a second, and the first change after a press of the gadget's own
key (or the card's Confirm) gets rc 0 (`muse_local_press.h`). A press at any
other time never counts. An upload's first chunk that names no whole-image
SHA-256, and a chunk with no upload started on its link, get rc 11
(`MG_SMP_EACCESSDENIED`) and no prompt. The press binds to that upload's
length and SHA-256 for 600 s, across link drops: the same upload starts again
or resumes on a new link with no new prompt, and the link that carries it
goes on (its chunks, the state write, an erase or the reset); a transfer under
way at 600 s finishes on its link, but starting it again asks again, as any
other upload does. A write past the bound length, a last chunk after which
the slot doesn't hash to the bound SHA-256, and a state write while it
doesn't get rc 11 and end the binding (the core's transfer is dropped,
`mg_smp_abort`). A press on a prompt an erase, a reset or a state write
started covers only that request on its link, for 600 s. Forget ends the
binding, what a press covered and the link's proof. The prompt's timeout or a
cancel (a touch elsewhere on the card) answers rc 11 once.
The ask wakes the screen; a passcode-locked gadget wakes to its keypad
("Unlock to update"), and the confirm card follows the unlock within the same
30 s. A SAFE the gadget entered itself
can't prompt (its screen is off): rc 11, the way back being the key into SAFE
or USB recovery. Refusals are logged on `mg.ble`; reads are never asked.
Every request, proven or not, nested deeper than 8 CBOR levels is refused
rc 3 before any decoder sees it (`mg_smp_front_depth_ok`, an iterative walk
that fails closed: a malformed or cut-short item, a length or count past the
request or past 32 bits, or bytes after the body are refused too; the core's
own skip stops at depth 8 and compares lengths in 64 bits, `smp/mg_smp.c`
`cr_head` and `cr_skip_body`). A request larger than the front holds (the
core's 1 KB slot) reaches the core as its header alone, which the core answers
EMSGSIZE; the front drops the rest of its bytes and never passes them on, so
nothing in them is read as a request of its own. With no memory for the FIFO
block, and so no front, SMP writes are refused. Any image upload write past its upload's
length (an offset-0 request longer than its own len, or a later chunk past the
len its first chunk named) gets rc 11 and ends the upload (`mg_smp_abort`),
however the link was opened, as smp-v12 does on a gated platform; this
smp-v11 core would drop the extra bytes and answer rc 0
(`mg_smp_front_past_len`). On a link the press opened, a chunk is carried on
only for the bound upload: while the core holds another one (an upload a
proven link started, held for its resume) the chunk gets rc 11 (smp-v12.1's
rule, `mg_smp_front_gate`).
The front costs 1 KB in the FIFO block while a link writes, 160 bytes for
the slot while a press binds an upload, and 40 bytes for the upload's length
and SHA-256 while the core holds an upload.

**On the gadget's screen** (`components/muse/muse_ui.c`'s card, from
`mg_smp_esp_ui()`): "Updating N%" with a neutral bar and the dim "Don't touch
it until it's done", "Installing" with "Don't remove power" from the
last byte on, and "Update stopped" for 4 s, its second line what the stop
left (`mg_smp_esp_stop_text()`): "Nothing was installed" for a stop
before the commit (a refused file, an upload given up, or the link gone
after the last byte but before the test: still the old version), and
"Installed; restart to finish" for the link gone after the commit with no
reset on its way (`mg_smp_esp_ui_installed()`: the image boots at the next
restart, pending its test boot). The link's end, not the core giving its
state back (it does that right after a commit too), is what stops a card
left at "Installing" (`mg_smp_esp_ui_session_gone()`, not while restarting
for the update). `test_mg_smp`'s UpdateStopLine drives each case through the
real backend.

## For clients

Things a phone app or other client has to get right beyond the header:

- **Write Without Response back-pressure.** stream_audio's buffer_update
  covers the device's playback buffer only. The client's own BLE stack also
  has a queue for Write Without Response, and on iOS a write it has no room
  for is dropped without an error. Before each Data write, wait for the stack:
  iOS `CBPeripheral.canSendWriteWithoutResponse` and
  `peripheralIsReady(toSendWriteWithoutResponse:)`; Android, one write at a
  time, the next after `onCharacteristicWrite` (API 33+: a
  `writeCharacteristic` that returns `ERROR_GATT_WRITE_REQUEST_BUSY` is
  retried after the callback); Web Bluetooth, await each
  `writeValueWithoutResponse`. `tools/mg_ble_client.py` does this with
  CoreBluetooth on macOS; without it macOS dropped 6 of 94 writes in a burst
  while the device saw none of them.
- **Count what the device says it received.** Bytes in flight are what you
  wrote minus buffer_update's total received; a write the device drops
  (not whole frames, or too big) still counts as received, so the two stay
  in step.
- **Check the speaker.** Read `speaker_volume` before playing: 0 means the
  device will play silence.
- **set_mic_gain is a level for this connection.** 1-100 (0 or over 100:
  `invalid_value`), with a fixed meaning: level 50 is the
  board's tuned gain (`MUSE_MIC_GAIN_TUNED`, 30 dB); 1-50 map linearly in dB
  from the codec's lowest step (0 dB) up to tuned, 50-100 from tuned up to
  the board's no-clip ceiling for speech at 30 cm
  (`muse_board_t.mic_gain_speech_max_db`; equal to tuned on a board with no
  headroom, every board until measured), in 3 dB steps (`mg_mic_gain.h`),
  applied at once (a running capture included, every format). It holds in RAM for this
  connection's captures only (`muse_audio_mic_gain_override()`): never
  saved, the user's Settings gain untouched; at disconnect, and on every new
  connection until it sends one, the gadget uses the user's or tuned gain.
  Send it only for a user's choice.

## Throughput test

`mg_command_device_action` with `mg_device_action_throughput_test`
(mgcommands.h, Throughput test; numbered as in the legacy protocol) is a bench
command on every build, listed with a typed list `[10 fe 1e 0b]`. Send has
the worker pump numbered MTU - 3 byte Data notifications as fast as the
stack takes them (up to 16 a round, every 5 ms) until the duration or stop;
receive counts the client's Data writes after its `change_data_type
throughput_test`, ending at the next one-second tick after the duration, or
on stop. Either reports `[1e 0b 00, bytes, packets, ms, gaps]`. It's `busy`
while audio is live, playing or claimed, start_mic is `busy` while it runs,
push-to-talk ends a send test (reported), and a disconnect ends either
unreported. `tools/mg_ble_client.py bench` drives it.

## Token proof

`mg_command_token_proof` (34; mgcommands.h, Token proof) is listed on every
gadget build. Link setup derives K = HKDF-SHA256(salt "mg token proof v1",
the access token as provisioned, the node id) and stores it beside the tokens
as `mg_proof_k` (hex) in Link's config; `main/mg_glue.c` hands it to this
component (`mg_ble_platform_t.proof_key`), which keeps it only for one HMAC.
A token refresh never changes it; reset or clear erases it.

- **challenge** [16-byte client nonce]: a fresh 16-byte device nonce from
  `secure_random` (refused `busy` without a radio on), then response [device nonce, HMAC-SHA256(K, "mg token
  proof v1 device" || client nonce || device nonce)]. The expected confirm is
  computed now, and K wiped. No key: `not_found`. A new challenge restarts.
- **confirm** [32-byte MAC]: compared in constant time, result 1 or 0; one
  confirm per challenge, else `invalid_value`.
- **clear** after result 1 on this connection: result 1, then the board
  erases the tokens, K and the setup markers and restarts into Link setup
  (`app_gadget_clear_setup()`). The sealed clear's answer goes out before
  the restart, which waits for it (at most 5 s, or the link's end), and the
  device session takes credit while the erase runs. Before a match: `proof_required`.
- A match keeps its nonces for one sealed device-session hello
  (`mg_proto_proof_take_nonces()`, which hands them out once and wipes
  them; a new challenge wipes them too). Nonces, the expected MAC and the
  match are wiped at every connect and disconnect.
- **What needs it** on a set-up gadget (`mg_proto_owner_ok()`; a gadget that
  isn't set up is open; a set-up one, its setup marker set or unreadable,
  whose K is missing or unreadable has no owner, so no proof can match):
  power off, the offline queue's erase, UI input's `hid_map` (kept in NVS)
  and `hid_key`, and a `set_settings` that
  names a stored setting (the buzz,
  `audio_codec`, `audio_queue_enabled`, `speaker_volume`, the capture rate
  and bitrate), which is refused whole with `proof_required` and nothing
  applied. Per-connection settings (`push_to_talk_enabled`,
  `haptics_enabled`, `link_mode`, `client_app`), `get_settings` and status
  stay open; the bench output switch doesn't count. `set_unix_time` sets the
  time from any connection, from 2024-01-01 on (Clock, above).
  `test_mg_ble`'s stored-settings row. Also:
  - **Streams to the gadget:** audio and video `start_streaming` (playback,
    video, UI streams) answer `proof_required` unless this connection's
    proof matched, or, on automation and test builds, the bench override is
    on (`bench.out`).
  - **`haptic_play`** on a release build answers `proof_required` without
    the proof (bench, automation and test builds: open).
  - **`start_mic`'s press rule:** on a release build of a set-up gadget a
    client's `start_mic` answers `press_required` unless the gadget's own
    press came within 5 s (`MG_PRESS_WINDOW_MS`, `mg_ble_press()`: its talk
    key, a finger on a talk surface, a screenless board's key); bench,
    automation and test builds are exempt. Before the proof, start_mic
    also needs an outstanding token-proof challenge on this connection.
  Firmware updates don't need it: an unproven link gets one confirm press on
  the gadget instead (Firmware update, Who may update).
- **The owner's content** on a release build of a set-up gadget answers only
  a connection whose proof matched: `get_unix_time` is `proof_required`;
  `get_settings` answers its status fields and skips `client_name` and
  `client_app` (then `proof_required` once); the Nordic UART log mirror stays
  silent until the proof (a new link waits for its own), except the answer
  to an identity or status read (`status`, `version`, `guard?`, `ota?`),
  which goes to the asking connection; its `!IN voice?` and `!IN img?` reads
  are refused with nothing sent back (logged on the gadget); the
  factory-test service doesn't notify the gadget's presses outside its open
  window. Identity and status reads, touch events and coordinates, and the
  writes stay as they are; test, bench and automation builds are unchanged
  (`tests/test_owner_reads.py` lists every such read).
- **Logs** never show a token or K: lengths and the first 4 bytes of a
  token's SHA-256 at most.

## Device session

`mg_command_session` (0x89; mgcommands.h, Device session) is listed on BLE-only
builds (`CONFIG_MUSE_GADGET_BLE_SESSION`, on with
`CONFIG_MUSE_GADGET_BLE_STANDALONE`): the gadget's own VM session, with the
phone as its network. The phone runs TLS, the WebSocket and Noise as the
gadget's node; the gadget runs what a Wi-Fi gadget runs on its session, from
the same code (`main/noise_control.cpp`, Device session over BLE): `POST
/link-control` and `link.register` when the session opens, the heartbeat,
`link.invoke` answered with `link.result`, and its own request streams
(`noise_ctrl_req_open()`: `GET /fs/raw`, `/chat/...`). Each ServiceFrame goes
out as one ServiceRequestEnvelope; each ServiceResponseEnvelope from the phone
is decoded and dispatched as a Wi-Fi gadget dispatches what Noise opened.

- **Caps:** request_status sends `[supported_features, sub_feature,
  mg_command_session, caps]` right after the command list: sealed (0x01)
  always, client_refreshes (0x02) on these BLE-only builds (no Wi-Fi: the
  app refreshes the pair and writes it back).
- **Wi-Fi gadgets** (BLE audio without `CONFIG_MUSE_GADGET_BLE_STANDALONE`)
  have no VM session over BLE; they run a credentials-only device session,
  `../../../portable/mg_device_session` layer 2 built from there unchanged,
  under `mg_proto.c` (`cfg.creds_session`, about 100 bytes allocated at
  init): caps sealed, plus client_refreshes while the gadget holds no Wi-Fi
  credentials (asked at each request_status: with Wi-Fi it refreshes its
  own pair), hello,
  sealed credentials (node id and the pair) and sealed credentials_update
  with layer 2's refusals and one sealed session per proof, every
  VM-session part refused or dropped (no open_request). The credential is
  read from Link's config only while a hello needs it, an update gets
  storage only while it arrives, and the pair is stored with the same
  `app_gadget_store_session_tokens()` (below).
- **hello** needs a matching token proof on this connection and a stored
  token (else `proof_required` or `not_found`), and the sealed flag (below):
  an unsealed hello gets `insecure` on any link, bonded or not (no Security Request). The answer is hello [1, max_message, window], both
  `CONFIG_MUSE_GADGET_BLE_SESSION_MAX_MESSAGE` (16 KB): a message is credited
  back when the session code takes it, so the window is the whole buffer.
- **Sealed credentials** (mgcommands.h, Sealed credentials): a hello with `mg_session_hello_flag_sealed` (its
  optional last byte) needs no bond. The device's hello echoes the flag and
  the credentials go sealed (`mg_session_flag_sealed`, total_len the sealed
  length: counter, ciphertext, 16-byte tag) under keys from K, the matching
  proof's nonces and the node id, with the shared seal
  (`../../../portable/mg_device_session`, `mg_session_seal.c`, built from
  there unchanged; `mg_seal_crypto.c` gives it HMAC-SHA256 and a
  constant-time compare on this component's PSA code). The nonces are taken
  after every other check, so a refused hello never spends a proof, and only
  once: **one sealed session per proof**, a second sealed hello before a new
  matching proof gets `proof_required` and the open session keeps its keys.
  A new challenge ends a sealed session. A client re-proves after any
  refusal (a new proof's session starts both counters from 0) and never
  seals again under a counter it has already sent. A plain session takes
  no sealed update.
- **Sealed data** (mgcommands.h, Sealed data):
  in a sealed session every message with a payload is sealed, data
  included, both ways; state, open_request, credit and abort never are.
  One counter per direction, shared by the credentials (device 0),
  credentials_updates and data, restarting only at a sealed hello (never at
  state open or a new epoch). A message is sealed whole, then fragmented,
  every fragment flagged `mg_session_flag_sealed`: total_len, the windows
  and credit count sealed bytes, and the plaintext is at most max_message −
  20 (`mg_session_client_max()` and `mg_session_room()` say so to the
  session code). Out: `mg_session_send()` is the one send point and seals in
  the queue's own copy of the message (+20 bytes, no other buffer). In, as
  portable layer 2 does it: the counter comes from the first fragment
  (`on_data()`, on the NimBLE host task: the next one or the hole below,
  else `invalid_value`), each fragment's ciphertext goes into layer 1's
  incremental tag (`mgseal_tag_begin` / `_update`, SHA-256 only; its ~110
  bytes are taken only while a message arrives) and the tag is checked at
  the last fragment (`mgseal_tag_check`), before the session code sees
  anything; the ring holds the sealed bytes, and `mg_session_take()`
  decrypts the message in place in the session code's own buffer, on the
  session task (`mgseal_xor_at`: about 500 HMAC-SHA256 blocks for 16 KB,
  off the host's stack). No sealed buffer of its own (the C6 has no PSRAM).
  A wrong tag gets `invalid_value`, the message dropped, its credit
  returned, and its counter **not** used: given back if nothing later took
  a counter, else left open **once** (two in
  flight: data and a credentials_update reassemble separately, so two can
  be in flight; the real message under it is accepted, a replay refused;
  one hole, a newer one replaces it). The rule is layer 1's
  `mgseal_rx_ok` / `_take` / `_unuse` (`mg_session_seal.h`), the one
  portable layer 2 runs, never a copy; an aborted message, or one dropped for a bad
  fragment or a state change, keeps its counter used. Unsealed data or
  credentials_update in a sealed session gets `insecure` and changes
  nothing (its credit returned).
  A sealed update's counter is taken at its first fragment too and the
  update opened with it later (on the worker with `store_later`); a wrong
  tag or the reflected direction get `invalid_value` and give the counter
  back, a sealed non-object uses it; nothing stored, no credentials_saved. The keys are allocated at the
  hello and live for the whole sealed session (its credentials, updates and
  data across every state open under it), wiped when it ends (a new hello,
  a reset, the connection). `test_mg_session` replays the seal vectors'
  `sealed_data` byte for byte and `sealed_interleaved` frame by frame (the
  update stored at once and with `store_later`) on the host reference and
  on PSA, and runs
  the relay placeholder and session.ping on a sealed session (a refused
  tag then the same counter accepted; an abort then counter + 1).
- **credentials** follow the hello: the node id, Link's `access_token` and
  `refresh_token`, and `device_key` / `device_key_id` with `vm_id` and
  `vm_kind` when setup stored them (`main/mg_glue.c`), compact JSON in the
  spec's key order, wiped once sent, then `access_expires_at` last when the
  gadget keeps one (as layer 2 sends it).
- **credentials_update** is a flat object: both tokens required, unknown
  members ignored, `access_expires_at` (unix seconds, mgcommands.h)
  kept only as an unsigned integer that fits 64 bits (any other form, or
  none: none kept), twice refused; anything nested is `invalid_value`.
  The pair and its `access_expires_at` (NVS `tok_exp`) go with
  `config_set_token_pair()`, in
  one commit through Link's re-provisioning journal
  (`main/config_store.h`), K and node_id untouched: a failed write rolls
  back, a power cut before the commit point leaves the old pair and expiry
  (boot recovers them), then credentials_saved 1, or 0 if it wasn't stored.
  In a sealed session whose hello carried
  `mg_session_hello_flag_sealed_saved` (1 << 1, echoed in the device's
  hello) that answer is sealed (vectors `sealed_credentials_saved`,
  `sealed_data_large`): framed as
  credentials (`[0x89, 8, first|last|sealed, total_len 29]`), plaintext
  `[1 || SHA-256(refresh_token)[0:8]]` or `[0 || zeros]`
  (`mgseal_saved_plaintext()`, layer 1), under the next device-to-client
  counter, the one the credentials and data share (`ctr_out`). Each sealed
  message takes its counter as its first fragment goes: a data message is
  queued in the clear and sealed in place then, so an answer queued after
  it but sent before it takes the earlier counter, and the client sees the
  counters in order; a data message already started keeps its own. An
  update superseded by the next before it was stored is answered sealed
  `[0]`. A plain session keeps the one-byte answer, and so does a sealed
  session whose hello lacked sealed_saved (a client that predates 5.7: an
  older iOS app, the bench app), exactly as before 5.7: the legacy
  unsealed `[session, credentials_saved, 0|1]`, superseded `[0]` included,
  taking no counter, so the device's next sealed data keeps the counter it
  would have had (a sealed answer to such a client puts its relay one
  counter behind, and it parked the session at every connection;
  `test_mg_session` replays the pre-5.7 `sealed_data` section for it).
  sealed_saved without sealed means nothing (no echo). Wi-Fi gadgets'
  credentials-only session gets the same from layer 2, including sealed
  `[0]` when it gives up on a held write-back (30 s, the storage taken back,
  none found) for a sealed_saved client; for any other it stays silent, as
  before 5.7.
  The expiry is handed back in the next credentials, so a cold handoff
  needn't refresh (and commit) on every connection. A setup's commit keeps
  none. A Wi-Fi gadget's own refresh keeps now + the response's `expires_in`
  (the access token's lifetime) in the same commit as its pair while its
  wall clock is set (nothing runs SNTP: a phone's set_unix_time sets it, on
  boards with `components/muse`), else the response's `Date` (a strict
  IMF-fixdate at 2024 or later; it sets nothing else) + `expires_in`, else
  none (`access_expires_from()` in `main/app.c`). Wi-Fi gadgets hand it back through layer 2's `expires()` /
  `commit_expires()` (`mg_proto.c`).
  The pair is stored on the worker outside the protocol lock and
  credentials_saved queued only if the session that sent it is still open. While a
  journal is open the credential is read from the pair it keeps aside, so a
  hello never hands out an old access token with a new refresh token. While
  the gadget is being set up again (the journal's begin until the commit
  has ended this session, or a rollback), updates are refused
  (`app_gadget_store_session_tokens()`): the client resends on its next
  connection.
- **A new setup's commit** (Gadget setup v2), like the token proof's
  `clear`, ends the session: `mg_ble_setup_changed()` resets the proof and
  the session, hello included (the connection's MTU stays), so the client
  proves the new key and says hello again.
- **state** open starts a session (the task in `noise_control.cpp` opens
  `/link-control`); anything else ends it, with every stream and pending
  result. Every state the client reports reaches `noise_control.cpp` once,
  a closed with no session open included: a failed open (connecting, then
  closed or no_network, none ever open; mg-session-v1 exchange 14) is an
  end the gadget didn't ask for, and the re-ask series continues from it
  (dropping that closed would leave a gadget whose open failed
  silent). `too_large` ends requests in flight with `NOISE_CTRL_REQ_TOO_LARGE`,
  as on Wi-Fi. A session that fails on the gadget's side (the VM resets or
  refuses `/link-control`) asks the client to close
  with open_request 0, then for a new one with open_request 1 once it has and
  Wi-Fi's reconnect backoff has passed (doubling from 2 s to 15 s while
  sessions keep failing within 30 s). Each session has an id
  (`mg_ble_session_id()`) that `mg_ble_session_send()` and `_take()` check
  under the protocol lock, so nothing of one session goes into the next even
  when the host task changes it mid-turn.
- **Data, credit, abort, state and credentials_update** writes run on the
  NimBLE host task, so none is lost to a full worker queue; hello (it reads
  NVS) and storing an update's whole pair go to the worker. Writes are taken
  up to ATT_MTU - 3 bytes, whatever the overlay asks.
  Notifications a full stack refuses (control answers such as the hello
  answer, credentials_saved and errors, then credit, credentials, fragments)
  wait and go in order from the worker.
- Messages from the VM wait in a ring of max_message bytes (taken when the
  client reports the session open, PSRAM where the board has some, given
  back when it ends or the link goes: nothing held between sessions, which
  matters on the C6, where it's internal RAM) until the session code takes
  them, as many as fit: only bytes are bounded, as the client's
  credit counts them. The session code takes them only while the result
  queue has room, so a burst of `link.invoke` backs the client off instead
  of losing results.
- Outbound messages queue up to 24 KB; the session code sends a control
  chunk or a request operation only while the largest envelope it makes
  fits, and control chunks fit the client's max_message (and the gadget's
  own). It never waits for the Wi-Fi path's internal DMA reserve
  (`noise_tx_has_dma_headroom()`, 16 KB for esp_tls; `tx_needs_dma_reserve`
  is false for `BleSessionOut`): the Watcher's BLE-only build idles near
  12 KB internal, and link.register would sit parked behind that check,
  silently, right after POST /link-control. Every allocation the session makes per
  message (out-queue copies, credentials, keys, the incremental tag) goes
  through `cfg.msg_alloc`, PSRAM first (`mg_ble.c`), as do the session task's
  buffers, the ring and, on BLE-only builds with PSRAM, cJSON's nodes
  (`cJSON_InitHooks` in `noise_ctrl_ble_start()`); nothing is sized by the
  client's max_message or window (`test_mg_session`'s heap mode: a 64 KiB /
  64 KiB client, plain and sealed, keeps nothing across sessions). The
  session logs the internal heap on `mg.session` at open, after
  link.register and at its end, and a message that can't go is logged
  before the session ends (open_request 0).
- **What nothing handles** (the Muse apps relay the session both ways, so
  whatever the VM sends back reaches the gadget): `ble_session_inbound()`
  takes every message whole, its bytes already credited back. A frame of an
  unknown kind, a Request frame from the VM, a frame on a stream the session
  doesn't run (anything but `/link-control` and an open daemon request:
  there's no tunnel or identity fetch over BLE, so a request that has ended,
  stream 2 or 3 or one never opened is nobody's) and a control message of a
  type it doesn't know (a reply nobody waits for, an unknown event or method)
  are logged by kind, stream, type and size, never content, and accepted. A
  malformed message (not a ServiceResponseEnvelope, an empty one, a frame
  that doesn't decode, a control message that isn't a JSON object, a
  `link.invoke` without an id) is refused the same way and dropped. Neither
  ends the session: each message arrives whole, so a bad one can't put the
  next out of step. Only what ends a
  Wi-Fi session still ends it: the control stream reset or refused.
- **session.ping**: `>session.ping` on the USB console, or `!IN session.ping`
  over the Nordic UART, sends one `{"method":"link.heartbeat"}`
  on the open session's control stream (sealed on a sealed session, as all
  its data is), the heartbeat a registered gadget
  sends anyway (no id, no reply to wait for; the VM only marks the node
  alive), after whatever control message is going out. It answers on
  `mg.session`: `sent link.heartbeat (27 bytes) on /link-control` (`, sealed`
  on a sealed session), `no session`, or `not registered yet` before the
  `link.register` reply.
- **Logs** never show the credential. The device session logs on
  `mg.session`, which the Nordic UART mirrors (`mg_ble_client.py log`): open,
  ended and failed, `link.register sent (N bytes)` and its reply
  (`registered`, `accepted, not registered` or `rejected, code C`, its size
  and how long after), what wasn't handled or was refused (data the seal
  refused too: `refused a data message (N bytes): a wrong counter`), and
  session.ping.

## Codecs

| | SBC (default) | LC3 |
|---|---|---|
| Frames | 16 kHz mono, 16 blocks, 8 subbands, loudness, bitpool 26: 60 B per 8 ms | 16 kHz mono, 10 ms, 40 B |
| Bit rate | 60 kb/s | 32 kb/s |
| Encoder, Xtensa (S3) | 2.6 MIPS | 7.5 MIPS |
| Encoder, RISC-V without FPU (C6, C5) | 2.0 MIPS | 85 MIPS |
| Flash | 3.7 KB | 35 KB |
| Offered | always | `CONFIG_MUSE_GADGET_BLE_LC3`, on by default on the S3 and ESP32 |

Each notification carries whole frames: as many as fit in ATT_MTU - 3, one
per notification at the minimum MTU of 100, four at 247. The loop sends what each 20 ms of audio produced, so latency stays under
about 30 ms plus the link.

## Offline clips

With a `mg_queue` partition (data, subtype `0x4D`; `partitions_muse.csv` has
1856 KB at the end of the 16 MB, after `files`, nothing moved: about 4.2
minutes of SBC or 7.9 of LC3 at 32 kb/s) and the client's
`audio_queue_enabled`, presses made while no client is connected are encoded
with the `audio_codec` setting, staged in PSRAM, and written as one clip when
released. The client lists and downloads them with `mg_command_audio_queue`.
Boards on `partitions.csv` or `partitions_muse_8mb.csv`, or updated over the
air from an older table, have no partition, and don't offer the queue. Clips
are stored in plain flash, like NVS without encryption.

Clips are the owner's voice: on a set-up gadget, `clip_info` and `read_clip`
answer `proof_required`, and `status` reports zero clips and zero used bytes
(capacity and the enabled flag stay real), until this connection's token
proof matched. When the pairing changes hands (Forget, a setup reset, a new
setup's commit) the log is gone at once, and the rest of the partition is
erased in the background, sector by sector (blank ones skipped), so no
earlier owner's audio stays readable on the flash. An NVS mark (`q_wipe`)
written before the first erase and cleared after the last lets a boot resume
a wipe a power cut or the reset's restart broke off; until it ends the queue
takes no clips.

## Nordic UART

TX mirrors log lines whose tag starts with one of
`CONFIG_MUSE_GADGET_BLE_NUS_LOG_TAGS` (default `mg,link.main,link.heartbeat,muse_audio`).
The service has no security, so the default leaves out tags that log what
people say. RX takes `status` and `version`.
