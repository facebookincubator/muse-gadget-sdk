# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Options shared by the app and the tests.

DT_ZEPHYR_USER := /zephyr,user

menu "Muse Gadget"

config MG_DEVICE_NAME_PREFIX
	string "Advertised name prefix"
	default "MuseGadget-"
	help
	  The GAP Device Name is this prefix followed by the last three bytes
	  of the identity address in hex, e.g. MuseGadget-1A2B3C. A product
	  in the Muse Gadget catalog keeps it and sets MG_DEVICE_PRODUCT_CODE.
	  The node id (gadget-1a2b3c; homelink-1a2b3c kept by a gadget
	  set up before) comes from the address, not the name.

config MG_DEVICE_MODEL_NAME
	string "Product name in link.register metadata.model_name"
	default ""
	help
	  The product name the agent names the gadget by
	  (link.register metadata.model_name, e.g. "XN54").
	  Empty: the catalog code (MG_DEVICE_PRODUCT_CODE); with neither, no
	  metadata.

config MG_DEVICE_PRODUCT_CODE
	string "Muse Gadget catalog product code"
	default ""
	help
	  The product's model code in the Muse Gadget catalog (uppercase
	  letters and digits, e.g. XN54). Set, the name is the prefix, the code,
	  a hyphen and the address bytes: MuseGadget-XN54-1A2B3C. Empty (SDK
	  boards): MuseGadget-1A2B3C. The whole name takes at most 25
	  characters (the scan response also carries the battery service
	  UUID; the build checks it); the node id doesn't change.

config MG_SDK_TOKEN
	string "Muse Gadgets SDK token"
	default ""
	help
	  Your mgst_ SDK token from gadgets.muse.ai (Account > SDK tokens).
	  Muse Link setup hands it to the app in pairing_confirmed, for the
	  device-token mint. Keep it out of committed config files.

config MG_SDK_TOKEN_SLOT
	bool "Take the SDK token from a slot an updater fills in the image"
	help
	  In place of CONFIG_MG_SDK_TOKEN: the image carries a 128-byte slot
	  (src/mg_token_slot.c, a marker then NULs) that an updater fills with
	  the owner's token and then signs the image again, so the image that
	  is built, stored and served never holds a token (a web updater can
	  write it in the browser). The marker left in place means no
	  token, as an empty CONFIG_MG_SDK_TOKEN does. AGENTS.md, SDK token slot.

config MG_SAFE_HOLD
	bool "A button route into SAFE mode (boot hold)"
	default y
	help
	  The default mg_board_safe_hold_allowed() allows the boot hold's SAFE
	  point: a cold-boot hold shows the SAFE cue at MG_SAFE_HOLD_MS, and a
	  release before MG_RESET_HOLD_MS boots once in SAFE mode (a longer
	  hold still resets setup). On a release build only such a SAFE takes
	  Bluetooth image updates from a central without the token proof with
	  no press; a SAFE the gadget entered by itself refuses them, set up or
	  not. A board can narrow the hook (e.g. only with its charger
	  present).

config MG_SAFE_HOLD_MS
	int "Boot hold to the SAFE point, where the board allows it (ms)"
	default 3000
	help
	  On a board whose mg_board_safe_hold_allowed() says so
	  (MG_SAFE_HOLD), a cold-boot hold shows the SAFE cue at this point,
	  and a release before MG_RESET_HOLD_MS boots once in SAFE mode.

config MG_RESET_RELEASE_MS
	int "Then release within this long to commit the setup reset (ms)"
	default 5000
	help
	  The setup reset commits on release: after MG_RESET_HOLD_MS
	  the LED flickers ("release now"); a release within this window
	  erases the setup, a longer hold does nothing.

config MG_RESET_HOLD_MS
	int "Hold the button this long at power-up to reset setup (ms)"
	default 5000
	help
	  Holding the talk button while powering up, and on for this long,
	  erases what Muse Link setup stored (the device tokens, the token
	  proof key and the setup markers) and advertises Link setup again.

config MG_SBC_BITPOOL
	int "SBC bitpool"
	range 2 124
	default 26
	help
	  SBC at 16 kHz mono, 16 blocks, 8 subbands, loudness allocation.
	  Bitpool 26 gives 60-byte frames every 8 ms (60 kb/s), the same
	  quality point as mSBC. At most 124: the encoder's bit allocation
	  divides by zero from 125 up. A live frame must fit one Data
	  notification: on a link whose room is smaller (bitpool 124 makes
	  256 B frames, a 247-byte MTU carries 244) live audio uses the largest
	  bitpool that fits, announced as such.

config MG_LC3
	bool "LC3 codec"
	default y
	help
	  Adds LC3 (16 kHz mono, 10 ms frames) to the start_mic codec list.

config MG_LC3_24K
	bool "LC3 capture at 24 kHz"
	depends on MG_LC3
	default y
	help
	  Lists 24000 next to 16000 for LC3 in the capture-rate list and
	  setting 13 (capture_sample_rate), encodes LC3 at 24 kHz when a
	  client asks for it, and captures it from the PDM at 32 kHz through
	  a 4:3 resampler (src/mg_resample.c; the nRF54L15's PDM has no exact
	  24 kHz setting).
	  liblc3 is built with its 16 and 24 kHz tables (LC3_SR_MASK=6). 16 kHz
	  stays the default; a client gets 24 kHz only when it asks. On since
	  the board check (XIAO): encode 1.12-1.27 ms per 10 ms
	  frame (at most 1.6 ms), the mic hears a played 1 kHz tone at 24 kHz
	  at the same level as at 16 kHz, and tones of 13-16 kHz played
	  into the mic leave nothing at their aliases above the room's noise.

config MG_CLIP_LC3_16KBPS
	bool "Offline clips in LC3 16 kHz at 16 kb/s"
	depends on MG_AUDIO_QUEUE && MG_LC3
	help
	  Records offline-note clips in LC3, 16 kHz, 16 kb/s (20 B per 10 ms)
	  instead of SBC (60 kb/s): about 6 minutes in the XIAO's 700 KB clip
	  store instead of about 1.6. clip_info carries
	  each clip's format. Off by default (clips in SBC); turn it on when
	  the apps you pair with decode clips by their clip_info.

config MG_LC3_MAX_KBPS
	int "Highest LC3 capture bitrate (kb/s)"
	depends on MG_LC3
	range 16 96
	default 64
	help
	  The top of the LC3 bitrate range listed in the capture list:
	  16 kb/s (20 B per 10 ms) up to this, at every LC3 capture rate.
	  Requests are clamped into it; 32 kb/s (40 B) is the default. 64 kb/s
	  keeps a whole second of pre-roll in the 7.5 KB backlog.

config MG_AUDIO_MAX_PACKET_MS
	int "Longest stretch of audio in one Data notification (ms)"
	default 40
	help
	  Whole codec frames are packed into each notification up to the ATT
	  payload size and this much audio, which bounds the added latency.

config MG_MIC_DEFAULT_GAIN
	int "The set_mic_gain level the tuned gain sits at (1-100)"
	range 2 99
	default 50
	help
	  The software gain applied to the PCM before encoding is the board's
	  tuned gain (4x, +12 dB) until a client sends set_mic_gain; this is
	  the level that tuned gain is reported and mapped at
	  (src/mg_mic_gain.h): lower levels fall to -30 dB at 1, higher ones
	  rise to CONFIG_MG_MIC_GAIN_TOP_DB at 100.

config MG_MIC_TUNED_Q10
	int "The board's tuned mic gain (Q10, 1024 = unity)"
	range 64 65535
	default 4096
	help
	  The software gain before encoding until a client sends set_mic_gain,
	  reported and mapped at CONFIG_MG_MIC_DEFAULT_GAIN (src/mg_mic_gain.h;
	  the mapped gain is capped at 16x). The XIAO's 4096 (4x, +12 dB) after
	  the PDM's own +10 dB puts speech at 30 cm near -20 dBFS.

config MG_MIC_GAIN_TOP_DB
	int "set_mic_gain 100: dB over the tuned gain"
	range 0 30
	default 12
	help
	  mgcommands.h: 100 is the highest gain at which normal speech 30 cm
	  away doesn't clip. Speech at 30 cm peaks near -20 dBFS at the tuned
	  gain on the XIAO, so +12 dB leaves it near -8 dBFS.

config MG_MIC_HPF_HZ
	int "DC-blocking high-pass corner (Hz, 0 = off)"
	range 0 200
	default 30
	help
	  A one-pole high-pass right after the microphone, before the gain:
	  PDM microphones start with a large DC offset that drifts for seconds.
	  Primed with the first sample at each start, so the first frames carry
	  no settling step.

config MG_DMIC_HW_GAIN_DB
	int "PDM hardware gain (dB, -20 to +20)"
	range -20 20
	default 10
	depends on NRFX_PDM
	help
	  The nRF PDM peripheral's own gain, in 0.5 dB steps from its 0 dB
	  default, applied before decimation so quiet speech keeps its
	  resolution. +10 dB with the default software gain puts speech from
	  about 30 cm near -20 dBFS on the XIAO nRF54L15 Sense.

config MG_HAPTICS
	bool "The board has a vibration motor"
	help
	  Only boards with a vibration motor list and accept haptics_enabled
	  and the ptt_buzz_* settings (mgcommands.h), so apps show vibration
	  controls only for them. The XIAO has none (its "buzz" is an LED
	  flash): off, those settings answer unsupported.

config MG_ASSISTANT_STATE
	bool "Show the assistant state on the LED"
	default y if $(dt_alias_enabled,led0)
	help
	  Lists mg_command_assistant_state and shows listening, thinking,
	  responding, done and error on the LED.

config MG_HOLD_MS
	int "Hold threshold of the talk button (ms)"
	default 250
	help
	  The hold contract: capture starts at key-down
	  (pre-roll), button_down goes out once the button has been held this
	  long, then the pre-roll (at most 1 s) as a burst, then live audio. A
	  shorter press is a tap: nothing is sent or kept.

config MG_SDK_TOKEN_BOARD
	bool "The SDK token comes from the board (mg_board_sdk_token())"
	depends on !MG_SDK_TOKEN_SLOT
	help
	  For a board whose loader holds the owner's SDK token: Link setup's
	  pairing_confirmed carries what mg_board_sdk_token() returns (the
	  board's own code) instead of CONFIG_MG_SDK_TOKEN, which is then
	  ignored. The slot's rules hold: at most 127 characters and a NUL,
	  else no token; only its length is ever logged. The board's code
	  keeps it the same way (never printed, never in the tree).

config MG_TAP_TO_TALK
	bool "Tap to talk: a press starts a turn, the next press ends it"
	help
	  For a board whose talk button can't be held (a reset supervisor on
	  the same line resets the board on a long hold): a press starts the
	  turn as a held press does (capture at key-down, button_down at the
	  hold threshold), its release changes nothing, and the next press
	  ends the turn (as a held press's release would; that press does
	  nothing else). A turn ends by itself after MG_TAP_TO_TALK_MAX_MS,
	  or when the client goes away. Link setup's confirmations keep their
	  own presses and holds.

config MG_TAP_TO_TALK_MAX_MS
	int "Tap to talk: a turn ends by itself after this long (ms)"
	depends on MG_TAP_TO_TALK
	default 30000

config MG_LED_IDLE_HEARTBEAT
	bool "Blink 20 ms every 4 s when idle (bench boards)"
	help
	  The LED is dark when idle. A bench board on USB may
	  want a sign of life instead; this adds a 20 ms blink every 4 s
	  whenever nothing else shows. Off by default.

config MG_STATE_COMPAT_TIMEOUT_MS
	int "Turn timeout for clients that send no assistant_state (ms)"
	depends on MG_ASSISTANT_STATE
	default 15000
	help
	  After stop_mic the LED shows thinking by itself. A client that has
	  sent no assistant_state on this connection and sends none within this
	  time gets done (then idle) instead: older clients never report the
	  turn. Any assistant_state cancels it for the rest of the connection.

choice MG_AUDIO_SRC
	prompt "Audio source"
	default MG_AUDIO_SRC_DMIC if $(dt_nodelabel_enabled,dmic_dev)
	default MG_AUDIO_SRC_SYNTH

config MG_AUDIO_SRC_DMIC
	bool "PDM microphone (DMIC API)"
	select AUDIO
	select AUDIO_DMIC

config MG_AUDIO_SRC_SYNTH
	bool "Synthetic test signal"
	help
	  A deterministic tone pattern at 16 kHz, for simulation and boards
	  without a microphone.

config MG_AUDIO_SRC_BOARD
	bool "The board's own audio source"
	help
	  The board's own code (a directory beside the app with an
	  app_ext.cmake, or anything else linked into it) provides
	  mg_audio_src_start(), mg_audio_src_read() and mg_audio_src_stop()
	  (src/mg_audio.h), for a microphone front end this app doesn't have
	  (an array with a beamformer, say). The link fails without them.

endchoice

config MG_AUDIO_QUEUE
	bool "Offline clip queue"
	default y if $(dt_nodelabel_enabled,audio_queue_partition)
	default y if BOOTLOADER_MCUBOOT && $(dt_nodelabel_enabled,slot1_partition)
	select FLASH
	select FLASH_MAP
	select CRC
	help
	  Records push-to-talk utterances to the audio_queue_partition (or
	  MCUboot's secondary slot, MG_QUEUE_IN_SLOT1) while no client is
	  connected (when audio_queue_enabled is set) and serves them with
	  mg_command_audio_queue.

config MG_QUEUE_IN_SLOT1
	bool "Keep offline clips in MCUboot's secondary slot"
	depends on MG_AUDIO_QUEUE
	default y if BOOTLOADER_MCUBOOT && $(dt_nodelabel_enabled,slot1_partition)
	help
	  The clip queue lives in slot1_partition, which firmware updates also
	  use: an upload hands it to OTA (dropping the clips), and the queue
	  takes it back once MCUboot needs nothing there (see src/mg_queue.h).

config MG_QUEUE_SLOT1_TRAILER
	hex "Bytes at the end of the slot the queue never writes"
	depends on MG_QUEUE_IN_SLOT1
	default 0x3000
	help
	  MCUboot's image trailer (magic, flags, swap status). With swap using
	  move it is 3 x (sectors per slot) x (write block) bytes of status plus
	  about 80 bytes: 3 x 178 x 16 + 80 = 8.6 KB for the XIAO's 712 KB slot
	  of 4 KB sectors, rounded up to whole sectors.

config MG_DFU
	bool "Firmware updates over BLE (MCUmgr SMP)"
	default y if BOOTLOADER_MCUBOOT && MCUMGR_TRANSPORT_BT && MCUMGR_GRP_IMG
	depends on BOOTLOADER_MCUBOOT
	select MCUMGR_SMP_COMMAND_STATUS_HOOKS
	select MCUMGR_GRP_OS_RESET_HOOK
	select MCUMGR_GRP_IMG_UPLOAD_CHECK_HOOK
	help
	  MCUmgr's image and OS groups on the SMP GATT service, for the Muse
	  apps, nRF Connect Device Manager and mcumgr clients: the BLE recovery
	  tier. On a release build, set up or not (a gadget with no device
	  token, not set up or after Forget, asks the same), an update write
	  (upload, image state, erase, reset) from a central without this
	  connection's token proof needs one physical press on the gadget (the
	  talk key included; an injected press never counts),
	  bound to the upload that asked (its length and whole-image SHA-256,
	  for 10 minutes; one that names no SHA-256 is refused, and so are
	  bytes past that length or that don't hash to it) or covering only
	  the change that asked, on its link; Forget ends it; the owner's
	  proven app, the button's SAFE and bench or automation builds need
	  none (src/mg_ota_policy.h). Reads and echo
	  stay open. A new image keeps itself
	  once its boot is healthy, and a client's confirm before then takes
	  effect at that point, so only an image that never gets healthy is
	  reverted at the next reset. Needs a sysbuild build with MCUboot.

config MG_SMP_GATE_BOARD
	bool "SMP over Bluetooth without MCUboot updates, gated with the board"
	depends on !MG_DFU && MCUMGR_TRANSPORT_BT && !MCUMGR_GRP_IMG
	depends on !MCUMGR_GRP_SETTINGS && !MCUMGR_GRP_FS && !MCUMGR_GRP_SHELL
	select MCUMGR_MGMT_NOTIFICATION_HOOKS
	select MCUMGR_SMP_COMMAND_STATUS_HOOKS
	help
	  For a board whose images go through its own loader (no image group
	  here) but which keeps MCUmgr's other groups over Bluetooth, such as
	  the OS group's reset into that loader. On a set-up gadget every SMP
	  command, read or write, needs this connection's token proof (in
	  SAFE mode, SAFE asked with the button), except the OS group's
	  harmless status (echo, task and memory pool statistics, MCUmgr's
	  parameters, OS and bootloader info; src/mg_smp_gate.c,
	  src/mg_ota_policy.h); then each write meets the board's own
	  mg_board_smp_write_allowed(), which has no default: a build without
	  it fails to link. Not with the image group, nor with the settings,
	  file system or shell groups (they would read or change stored data
	  on a gadget that isn't set up, which has no proof to ask). The
	  build refuses SMP over Bluetooth with neither this nor MG_DFU.

# Room after the SMP transport's own user data for each request's source
# connection (src/zephyr/smp.c change 3 with MG_DFU, src/mg_smp_req_gen.c with
# MG_SMP_GATE_BOARD): 8 bytes of smp_bt's on 32-bit, 16 on 64-bit
# (native_sim), plus 4.
config MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE
	default 24 if MG_DFU || MG_SMP_GATE_BOARD

# MG_DFU_RESET_DISCONNECT_MS, MG_SMP_SESSION_IDLE_MS, MG_SMP_RX_GAP_MS and
# MG_BENCH_NO_CONFIRM are mirrored in tests/smp_link/Kconfig (its native_sim
# build has no MG_DFU): change both together.
config MG_DFU_RESET_DISCONNECT_MS
	int "Delay from an SMP reset's answer to dropping the link (ms)"
	depends on MG_DFU
	default 100
	range 20 400
	help
	  After answering an SMP reset the gadget stops advertising and drops
	  the link itself, this long after the answer (time for it to go out),
	  then restarts at CONFIG_MCUMGR_GRP_OS_RESET_MS. Must be shorter than
	  that.

config MG_SMP_SESSION_IDLE_MS
	int "Idle time after which SMP's work queue is freed (ms)"
	depends on MG_DFU
	default 2000
	help
	  MCUmgr's work queue (src/zephyr/smp.c) starts with the first SMP
	  request, its stack from the malloc arena, and is stopped and freed
	  once no request or buffer has been about for this long
	  (update support takes no base memory). An upload that stopped
	  (a dropped link) frees its image writer then too, and resumes from
	  what is on flash.

config MG_SMP_RX_GAP_MS
	int "Gap after which a partly received SMP request is dropped (ms)"
	depends on MG_DFU
	default 2000
	range 0 10000
	help
	  An SMP request spans several ATT writes. When a write comes more
	  than this long after the previous one, a request still waiting for
	  bytes is dropped first: one of its writes was lost, and the client
	  is retrying (Android after 5 s, iOS after 30 s). Without it Zephyr's
	  reassembly appends every retry to the stale request until the link
	  drops. 0 turns it off.

menu "Connection parameters"

config MG_CONN_INTERVAL_MIN
	int "Connection interval, minimum (1.25 ms units)"
	range 6 3200
	default 12
	help
	  12 is 15 ms. Apple's accessory rules: at least 15 ms, and the maximum
	  at least 15 ms above the minimum unless both are 15 ms.

config MG_CONN_INTERVAL_MAX
	int "Connection interval, maximum (1.25 ms units)"
	range 6 3200
	default 12

config MG_CONN_LATENCY
	int "Peripheral latency in the fast mode (connection events)"
	range 0 30
	default 0
	help
	  Link modes (mgcommands.h): active is 12/12/0/400, latency 0, only
	  while the gadget is active (capture, a talk press, Link setup, an
	  update, a clip download) or the client wrote active. Apple: at most 30, and
	  interval max x (latency + 1) x 3 below the timeout.

config MG_CONN_TIMEOUT
	int "Supervision timeout (10 ms units)"
	range 10 3200
	default 400
	help
	  400 is 4 s (Apple: 2 to 6 s).

config MG_CONN_IDLE_INTERVAL_MIN
	int "Ready mode: connection interval, minimum (1.25 ms units)"
	range 6 3200
	default 24
	help
	  Link modes (mgcommands.h): ready is 24/36/5/400 (30-45 ms, latency 5,
	  4 s), at connect and 5 s after the last activity; it never allows
	  15 ms (a Mac asked 12/24 on a 15 ms link sends no update at all). If
	  set to allow 15 ms, a central that grants 15 ms, or sends no update
	  within 2 s, moves it to 24 and up.

config MG_CONN_IDLE_INTERVAL_MAX
	int "Ready mode: connection interval, maximum (1.25 ms units)"
	range 6 3200
	default 36

config MG_CONN_IDLE_LATENCY
	int "Ready mode: peripheral latency (connection events)"
	range 0 30
	default 5

config MG_CONN_IDLE_AFTER_MS
	int "Back to the client's mode this long after the last activity (ms)"
	default 5000
	help
	  Link modes (mgcommands.h): 5 s after the device's last activity ends
	  (a capture, a press, a clip download, an update, Link setup).

config MG_CONN_SLEEP_INTERVAL_MIN
	int "Sleep mode: connection interval, minimum (1.25 ms units)"
	range 6 3200
	default 100

config MG_CONN_SLEEP_INTERVAL_MAX
	int "Sleep mode: connection interval, maximum (1.25 ms units)"
	range 6 3200
	default 160
	help
	  100/160/4/600 (mgcommands.h, "Link modes"): 200 left 3.3
	  listens of slack at a 240 ms grant, and a gadget dropped 11 times
	  overnight in sleep.

config MG_CONN_SLEEP_TIMEOUT
	int "Sleep mode: supervision timeout (10 ms units)"
	range 10 3200
	default 600

config MG_CONN_SLEEP_LATENCY
	int "Sleep mode: peripheral latency (connection events)"
	range 0 30
	default 4

config MG_CONN_SLEEP_AFTER_MS
	int "Sleep by itself after this long in ready without activity (ms)"
	default 600000

config MG_CONN_CLIENT_ACTIVE_MS
	int "A client's active write lapses after this long without activity (ms)"
	default 30000

config MG_SETUP_DEVICE_KEY_AUTH
	bool "Link setup's get_device_info offers device-key auth"
	help
	  Off: a
	  BLE-only Link setup must not offer device-key auth: the app's setup
	  mints a device key when it's offered, the key stays on the
	  gadget, and the phone then declines to relay the gadget's session
	  (not_enrolled). Device-key auth is for setups that provision Wi-Fi,
	  which this gadget never does. provision_v2 still takes a device key
	  if one comes. The automation console's >keyauth=0|1 changes it at
	  run time for a test.

config MG_PTT_POSTROLL_MS
	int "Push-to-talk: capture this long after the release (ms)"
	range 0 1000
	default 200
	help
	  After a live turn's button_up the mic keeps capturing
	  this long (people let go on the last syllable), then the last frame
	  and stop_mic; the capture light stays until capture stops. A turn the
	  device ends (the client's stop_mic, a disconnect) stops at once, and
	  a new press ends a post-roll still running.

config MG_CONN_SWITCH_LISTEN
	bool "Hear every connection event during a link mode switch"
	default y if BT_LL_SW_SPLIT
	help
	  From a mode request until the central
	  applies it, the peripheral latency skip is cancelled twice an
	  interval (Zephyr's open controller; with the nRF Connect SDK's
	  SoftDevice Controller, its peripheral latency mode is set to disable
	  and back, src/mg_ble.c), then latency resumes. On the
	  XIAO with a Mac central (10 switches each): sleep -> active median
	  6.8 s without, 3.4 s with; sleep -> ready 4.2 s -> 3.5 s; about 11
	  extra events heard per switch. On by default with Zephyr's open
	  controller only: on the SoftDevice Controller
	  (an nRF54L15 board, 10 rounds each) it gained nothing, both ways at the
	  central's 3.2 s floor (the SDC hears the update by itself), and costs
	  27-51 every-event wakes a switch; a board there can still set it y.
	  The bench console's ">listen=0|1" and ">mode=0|1|2" (the client's
	  link mode, as if written; the Nordic UART's "listen" and "mode" too)
	  drive it at run time.

config MG_ADV_FAST_SECS
	int "Fast advertising after boot, a disconnect or a press with no app (s)"
	default 30
	help
	  20-30 ms advertising (Apple's first 30 s) for this
	  long after boot, after a disconnect (from the link's loss) and after
	  a press with no app connected; then CONFIG_MG_ADV_SLOW_MS.

config MG_ADV_SLOW_MS
	int "Slow advertising interval afterwards (ms, 0 = always fast)"
	default 1022
	help
	  One of Apple's recommended longer intervals (1022.5 ms): found by a
	  phone in the background within a few seconds, at a fraction of the
	  fast interval's power. Before setup (Link setup advertised) the
	  gadget always advertises fast.

config MG_CONN_REQ_GAP_MS
	int "At most one connection parameter request this often (ms)"
	default 2000
	help
	  The fast and idle modes are each a real update, at
	  most one request per this many ms.

endmenu

menu "Recovery and diagnostics"

config MG_WATCHDOG
	bool "Hardware watchdog over the gadget's threads"
	default y if $(dt_alias_enabled,watchdog0) && !SOC_SERIES_BSIM_NRFXX
	select WATCHDOG
	help
	  The watchdog0 WDT (WDT31 on the XIAO) resets the
	  gadget unless a supervisor on the system work queue feeds it, which
	  it does only while the app work queue answers a ping and the audio
	  and Bluetooth TX threads, when working, keep beating. A stuck one
	  leaves a breadcrumb ("watchdog: audio thread stuck") for the next
	  boot. It starts at the first moments of a boot (PRE_KERNEL_2) and,
	  once running, keeps running across a soft reset: after an SMP reset
	  the bootloader's work and the new image's boot to that early start
	  must fit one window (fed last just before the reset, and again at
	  the early start). The XIAO's MCUboot feeds it while it swaps
	  (CONFIG_BOOT_WATCHDOG_FEED); a board whose bootloader doesn't (an
	  overwrite-only MCUboot built without it, say) sets
	  CONFIG_MG_WDT_BOOT_COPY_KBPS so the build checks its slot copy fits.

config MG_WDT_TIMEOUT_MS
	int "Watchdog timeout (ms)"
	depends on MG_WATCHDOG
	default 10000

config MG_WDT_BOOT_COPY_KBPS
	int "The bootloader's slot copy rate when it doesn't feed the watchdog (KB/s)"
	depends on MG_WATCHDOG
	default 0
	help
	  0: the bootloader feeds a running watchdog (or copies nothing).
	  Otherwise the measured rate at which it copies an update into the
	  primary slot without feeding it; the build fails unless a whole
	  slot0 at that rate plus CONFIG_MG_WDT_BOOT_MARGIN_MS fits inside
	  CONFIG_MG_WDT_TIMEOUT_MS (raise the timeout, which only the first
	  start after a power-on or watchdog reset can set).

config MG_WDT_BOOT_MARGIN_MS
	int "Margin for the bootloader's start and the boot to the early watchdog (ms)"
	depends on MG_WATCHDOG
	default 2000

config MG_WDT_STALE_MS
	int "A working thread that hasn't beaten for this long is stuck (ms)"
	depends on MG_WATCHDOG
	default 8000

config MG_HEALTHY_AFTER_MS
	int "Healthy this long after advertising starts (ms)"
	default 30000
	help
	  A boot counts as healthy once BLE has been up and advertising this
	  long with the watchdog's threads alive: the failed-boot count starts
	  over, and an image on test (a firmware update) is confirmed then, not
	  as soon as advertising starts, so one that crashes in its first
	  seconds goes back to the previous image at the next reset.

config MG_BOOT_RAM_END
	hex "End of the RAM the board's shipped bootloader uses"
	default 0x0
	help
	  The first address above every RAM byte the board's shipped
	  MCUboot touches (its .data, .bss, stacks and heap: _image_ram_end in
	  its map). Nonzero, the build requires the devicetree block
	  mg_retained (zephyr,memory-region, at the top of SRAM, outside
	  zephyr,sram) for the boot record (unhealthy-boot count, crash
	  breadcrumb) and fails if that block starts below this. 0: unchecked,
	  the record in .noinit.

config MG_SAFE_MODE_BOOTS
	int "SAFE mode after this many boots in a row never got healthy"
	default 3
	help
	  The crash-loop escape: the boot after this many
	  unhealthy ones (crashes, watchdog resets, resets before healthy) runs
	  in SAFE mode: Bluetooth, the mg service's status, the Nordic UART
	  console and SMP firmware updates only; no audio, clips, push-to-talk,
	  Link setup or bench console. A healthy SAFE boot clears the count, so
	  the next reset tries the normal image again. 0 turns it off.

config MG_REACH_RESET
	bool "Restart a boot that isn't reachable"
	default y if !SOC_SERIES_BSIM_NRFXX && !ZTEST
	select REBOOT
	help
	  src/mg_reach_policy.h: a boot with no connectable
	  advertising or link 30 s after it starts, or 30 s without either once
	  it was reachable, restarts as a failed boot (losses counted across
	  boots, so three in a row reach SAFE); inside SAFE, after three such
	  restarts in a row it stays up. Off in simulations.

config MG_FATAL_REBOOT
	bool "Leave a breadcrumb and restart on a fatal error"
	default y if !SOC_SERIES_BSIM_NRFXX && !ZTEST
	select REBOOT
	help
	  Replaces Zephyr's fatal error handler: the fault, PC, thread and
	  uptime go into the boot record (logged and kept at the next boot,
	  NUS console "reset") and the gadget restarts at once instead of
	  halting until the watchdog fires.

endmenu

config MG_BENCH_BOARD_NOT_READY
	bool "The board never reports ready (board-ready test)"
	depends on MG_AUTOMATION || MG_BENCH_MODE
	help
	  Bench builds only: mg_board_ready()'s default says not ready, so the
	  boot never counts healthy (a test image reverts, three boots reach
	  SAFE), standing in for a board init that blocked. Never ship it.

config MG_BENCH_EARLY_HANG
	bool "Hang before the watchdog supervisor starts (early-watchdog test)"
	depends on MG_WATCHDOG && MG_AUTOMATION && BOOTLOADER_MCUBOOT
	depends on !MCUBOOT_BOOTLOADER_MODE_OVERWRITE_ONLY
	help
	  Bench builds only (MG_AUTOMATION): the boot hangs after the
	  watchdog's early start and before its supervisor, so the watchdog
	  must reset the gadget. Install it as a test image only (MCUboot then
	  swaps the previous one back); never ship it. Not offered with an
	  overwrite-only MCUboot, where a test image stays for good.

config MG_BENCH_MODE
	bool "Bench mode: every human step scriptable (never ships)"
	help
	  Orthogonal to MG_AUTOMATION, so a release-configured image can run
	  its release gates end to end with no hand on the gadget. With it on:
	  an injected press counts as the button for every human gate (the
	  mic's press window, the setup and re-setup confirm); the
	  bench switch over Bluetooth needs neither a press nor a proof (per
	  connection, off at disconnect); a bench SAFE request and a setup
	  reset request (USB console `>bench.safe` / `>bench.reset`, Nordic
	  UART `safe next` / `reset next`) take effect at the next boot as the
	  button's boot hold would (declaration reason bench); the reach, ATT
	  dead and TX dead bench rows run; and the human steps are also on the
	  Nordic UART for boards without a console (`talk down`, `talk up`,
	  `pair confirm`). The image id gets its bench bit and
	  the firmware string ".bench"; tools/check_mic_gate.py --release
	  refuses the image (symbol mg_bench_mode_image), so it never ships.

config MG_BENCH_BUILD
	bool "(old name of MG_BENCH_MODE)"
	imply MG_BENCH_MODE
	help
	  The bench marker's old name, kept for overlays that still set it.

config MG_BENCH_NO_CONFIRM
	bool "Never confirm a new image (revert test)"
	depends on MG_DFU
	depends on !MCUBOOT_BOOTLOADER_MODE_OVERWRITE_ONLY
	help
	  Bench builds only: the app doesn't confirm an image running on test,
	  so MCUboot swaps the previous image back at the next reset. Build a
	  test image with it to show a revert; never ship it.

config MG_UPDATE_KEY
	bool "A dedicated update key answers the update prompt"
	depends on MG_DFU
	help
	  The update prompt (an update write from a central without the token
	  proof) takes one press of the talk button; a board with a dedicated
	  update key can answer it with that key instead, one press. Off by
	  default: the XIAO has one key, its talk key. A board with such a key
	  turns this on and calls mg_core_update_key() from its input callback.

config MG_AUTOMATION
	bool "Automation tools (bench and CI builds; off in release builds)"
	help
	  One switch for the automation tools: the setup rig's @pair.pending
	  line and ">pair.confirm" press on the USB console, and the Nordic
	  UART console's bench words open to any connection. Release builds
	  have it off; overlay-bench.conf turns it on for lab and CI builds.
	  Off, the Nordic UART's state-changing words need a token-proven
	  connection (src/mg_nus_policy.h) and ">pair.confirm" answers
	  "@pair.confirm disabled"; the USB console's other words stay (MG_BENCH).
	  The recovery tiers (SMP over BLE, SWD, the button) are in every build
	  either way.


config MG_SILENT_TURN_DROP
	bool "Drop talk turns that never rise above room noise (test builds)"
	depends on MG_AUTOMATION
	default y
	help
	  Bench safety: a test or
	  automation build holds a talk turn's output (its gestures too) until
	  the capture rises above room noise, and drops a turn that never does
	  unsent, with the no-result cue (4 blinks), so a scripted hold never
	  sends a silent or room-only turn. Counted in the Nordic UART status
	  (silent=). Release builds (MG_AUTOMATION off) send every hold.
config MG_BENCH
	bool "Bench console"
	depends on UART_CONSOLE && SERIAL_SUPPORT_INTERRUPT # console_getchar()
	default y
	select CONSOLE_SUBSYS
	help
	  Single-key commands on the console UART (the XIAO's USB serial port),
	  in every build, for testing without a phone or a hand on the button:
	  d / u press and release the talk button through the real button path
	  (a press while a setup confirmation waits confirms it), s prints the
	  status, l sends a test message on an open device session, and lines
	  (>bench=, >listen=, >mode=, >expire, >keyauth=). The line
	  ">pair.confirm" is the button's setup press with MG_AUTOMATION.

config MG_AUDIO_QUEUE_MAX_CLIPS
	int "Most clips the queue holds"
	depends on MG_AUDIO_QUEUE
	default 128

config MG_BATTERY_ADC
	bool "Read the battery voltage with the ADC"
	default y if $(dt_node_has_prop,$(DT_ZEPHYR_USER),io-channels)
	select ADC
	help
	  Without it, Battery Level reads 100.

config MG_BATTERY_SHUTDOWN_MV
	int "Power off cleanly at this battery voltage (mV)"
	depends on MG_BATTERY_ADC
	default 3300
	help
	  Two readings in a row at or under this voltage (with
	  a cell present: 2.5 V or more) power the gadget off (System OFF, the
	  button wakes it) before the cell browns out. 3.3 V is a single LiPo
	  near empty under the gadget's light load (0 % on the curve), leaving
	  margin above the cell's protection cut-off (about 2.8-3.0 V) and above
	  where the board's 3.3 V regulator stops regulating. Never applies on
	  USB without a cell (that reads under 2.5 V or as the charger's 4.2 V).

config MG_BATTERY_STATUS
	bool "device_action request_battery_status / report_battery_status"
	default y
	help
	  Lists device_action request_battery_status (8) and answers it with
	  report_battery_status (9): the last reading's voltage and charge when
	  a cell is present (flag present), every value the board can't measure
	  0; a board without a battery reading answers present 0 (every
	  gadget that takes device_action answers it; the apps prefer it over
	  BAS). The report also goes unsolicited when a
	  cell appears or goes, every 5 % of charge since the last report, and
	  when 15 % or 10 % is crossed.

config MG_BATTERY_RESUME_MV
	int "A low cell's boot waits for this voltage (mV)"
	default 3500
	help
	  A boot with a cell under this voltage charges dark
	  on USB (no radio, LED off) until the cell reaches it, then starts;
	  without USB it stays off (a plug or the button wakes it). Above
	  CONFIG_MG_BATTERY_SHUTDOWN_MV, so a cell that just powered off
	  doesn't boot only to power off again.

config MG_POWER_OFF
	bool "device_action power_off (System OFF, the button wakes)"
	default y if (SOC_SERIES_NRF54L || SOC_SERIES_NRF54LX) && $(dt_alias_enabled,sw0) && !SOC_SERIES_BSIM_NRFXX
	select POWEROFF if HAS_POWEROFF
	help
	  Lists device_action power_off (10): the gadget drops the link, darkens
	  the LED and enters System OFF; a press of the talk button wakes it
	  (it boots as after a reset).

config MG_BATTERY_DIVIDER_RATIO
	int "Battery divider ratio (VBAT / pin voltage)"
	depends on MG_BATTERY_ADC
	default 2

config MG_NUS
	bool "Nordic UART Service log mirror and console"
	default y
	select BT_ZEPHYR_NUS
	select RING_BUFFER

config MG_SETTINGS_KEEP_ON_READ_ERROR
	bool
	default y
	depends on SETTINGS_ZMS
	select SETTINGS_ZMS_NO_LL_DELETE
	help
	  ZMS's settings listing deletes an entry whose name or value it can't
	  read, a transient read error included, which could take the setup's
	  record or a slot entry with it. With SETTINGS_ZMS_NO_LL_DELETE it
	  skips such an entry and deletes nothing (the setup store then finds
	  the listing partial: unreadable, never absent); a deleted entry's list
	  node stays and is used again when that name is stored again.

config MG_LINK_SETUP_CRYPTO
	bool
	default y
	select PSA_CRYPTO
	select PSA_WANT_ALG_ECDH
	select PSA_WANT_ECC_SECP_R1_256
	select PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE
	select PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT
	select PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_EXPORT
	select PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY
	select PSA_WANT_ALG_SHA_256
	select PSA_WANT_ALG_HMAC
	select PSA_WANT_KEY_TYPE_HMAC
	select PSA_WANT_ALG_GCM
	select PSA_WANT_KEY_TYPE_AES
	help
	  Muse Link setup (P-256 ECDH, HKDF-SHA256, AES-256-GCM) and the
	  token proof (HMAC-SHA256), on PSA Crypto.

config MG_VM_SESSION
	bool "The device session: the gadget's own VM session over BLE"
	default y
	help
	  mg_command_session (mgcommands.h, Device session): after a matching
	  token proof and a sealed hello the gadget hands the app its
	  credentials, sealed under keys from the proof (the link itself isn't
	  encrypted: BT_SMP=n), and the app carries the gadget's own Noise session to
	  its owner's VM. On it the gadget opens /link-control, registers its
	  device commands (mg_vm_commands.c) and answers link.invoke.
	  supported_features lists mg_command_session.

if MG_VM_SESSION

config MG_VM_SESSION_MAX_MESSAGE
	int "Largest session message the gadget takes (bytes)"
	range 4096 65536
	default 16384
	help
	  The device's hello max_message, and the size of its receive buffer
	  (RAM). A VM message larger than this closes the session (too_large).

config MG_VM_SESSION_WINDOW
	int "Session receive window (bytes)"
	range 4 65536
	default 16384
	help
	  The device's hello window, at least 4 (its hello is always sealed,
	  and a sealed hello's window holds a whole counter: mgcommands.h,
	  Opening) and at most MG_VM_SESSION_MAX_MESSAGE. The
	  gadget gives a message's bytes back once it has handled all of it;
	  those of a message larger than the window (which could otherwise
	  never finish) as its fragments land, since the receive buffer
	  holds a whole message either way.

config MG_VM_SESSION_TX_QUEUE
	int "Session send queue (bytes)"
	default 4096
	help
	  Device -> client messages waiting for the client's credit.

config MG_VM_LINK_CONTROL_MAX
	int "Largest /link-control message, either way (bytes)"
	default 2048
	help
	  link.register (with every device command's description) and
	  link.result go out in one; a longer link.invoke from the VM is
	  skipped.

config MG_VM_LINK_COMMANDS_MAX
	int "Most device commands"
	default 8

config MG_VM_LINK_HEARTBEAT_S
	int "link.heartbeat interval (s)"
	default 86400
	help
	  After a "registered" reply, as a Wi-Fi gadget
	  (NOISE_CTRL_HEARTBEAT_INTERVAL_US).

config MG_VM_KEEPALIVE_MS
	int "Keepalive while a task is active (ms)"
	default 15000
	range 0 20000
	help
	  While the gadget has an active task (a user action in the
	  last 2 min, traffic owed, a board's wait, its own open request) and
	  its session is open, an empty frame on the open /link-control stream
	  goes this often (at most 20 s, under iOS's
	  ~30 s suspension of an idle backgrounded app). None with no task:
	  the session closes, and that's normal. 0: none.

config MG_NODE_ID_GADGET
	bool "New setups offer the node id gadget-<6 hex>"
	default n
	help
	  A new setup offers gadget-1a2b3c
	  instead of homelink-1a2b3c, derives K with it and stores it with K;
	  a gadget set up before keeps the id it was set up with until it is
	  set up again. Off by default; turn it on when the apps you pair with
	  classify gadget nodes by family. Bench builds turn it on
	  (overlay-bench.conf).

config MG_SESSION_SEALED_CLEAR
	bool "Forget's clear only inside the sealed session (mg_session_clear)"
	default n
	help
	  Declares mg_session_cap_sealed_clear, so the apps send Forget's clear
	  as the sealed mg_session_clear (12, taken in every build), and
	  refuses the unsealed token proof clear with insecure, so a relayed
	  proof can't let a proxy erase the gadget. Off by default; turn it on
	  when the apps you pair with send the sealed clear.

config MG_VM_IDLE_CLOSE_MS
	int "Close the session this long after the last task (ms)"
	default 20000
	help
	  With no active task for this long the
	  gadget closes its own session (open_request [0]) rather than leave
	  a silent one open (Android holds the VM socket with pings while a
	  session is open, keeping the VM awake). A new task asks again. Off
	  with MG_VM_KEEPALIVE_MS 0.

config MG_VM_COMMAND_LED
	bool "The device.led command"
	default y if $(dt_alias_enabled,led0)

endif

config MG_BSIM_TESTS
	bool "BabbleSim test hooks"
	depends on SOC_SERIES_BSIM_NRFXX
	help
	  Adds the bstest entry points used by tests/bsim to press the button
	  from the simulation.

module = MG
module-str = Muse Gadget
source "subsys/logging/Kconfig.template.log_config"

endmenu
