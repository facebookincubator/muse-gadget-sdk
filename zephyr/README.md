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

# Zephyr Device SDK

**Experimental:** BLE gadget support is experimental while the Muse apps
gate it.

A push-to-talk Bluetooth gadget for Muse, on the **Seeed XIAO nRF54L15 Sense**.
Hold the button and talk: the gadget streams your voice to the Muse app over
Bluetooth LE. With no phone around, it keeps what you say and hands it over
the next time the app connects.

It speaks the **musegadgets** BLE protocol in [`../protocols`](../protocols),
so the same Muse app that talks to the ESP32 gadgets talks to this one. The
firmware is a plain [Zephyr](https://zephyrproject.org) application: port it
to another Zephyr board with a button, an LED and a PDM microphone.

> **Note:** This is for experienced developers who are comfortable hacking on
> device firmware and using SDKs.

## What you need

- **A Seeed XIAO nRF54L15 Sense** (the Sense variant has the microphone).
  The plain XIAO nRF54L15 builds too, with a test tone instead of your voice.
- **A USB-C cable** that carries data.
- **A computer** running macOS or Linux, with the Zephyr toolchain (see
  [`AGENTS.md`](AGENTS.md) for the exact versions and commands).
- **The Muse app** on your phone.

## What it does

- **Push-to-talk.** Hold the user button: the LED lights and the microphone
  starts at once; after a quarter second the app gets a button-down gesture
  and that first quarter second as a burst, then the microphone streams
  16 kHz mono audio until you let go (a shorter tap sends nothing) (DC-blocked, with the PDM's own gain at +10 dB so speech from
  arm's length lands near -20 dBFS). The app can also start and stop the
  microphone itself (on a set-up gadget's release build, within 5 s of a
  press of the button).
- **Codecs.** SBC by default (16 kHz mono, 60 kb/s), LC3 on request (32 kb/s).
  Both are optimized builds of Google's open source codecs, in
  [`../xplat`](../xplat) (one copy, shared with the ESP32 SDK).
- **Offline clips.** Turn on the audio queue from the app, and utterances you
  record while no phone is connected are saved to the board's flash (about
  90 seconds of SBC) until the app downloads and clears them. They live in
  the spare firmware slot, so a firmware update drops them: download first.
  They are the owner's: on a set-up gadget only a phone that passed the
  token proof sees how many there are or reads them. Setting the gadget up
  again, or forgetting it (the app's Forget, the reset hold), drops them at
  once and then erases the whole clip store in the background, resuming
  after a power loss, so none of the earlier owner's audio stays in flash;
  the queue records again once that is done.
- **Firmware updates over Bluetooth.** MCUboot with test, confirm and revert:
  nRF Connect Device Manager (or any MCUmgr client, or
  `esp32/tools/mg_ble_client.py dfu`, or the Muse app's firmware update)
  uploads a signed image; it needs no phone: on a release build, without the
  token proof on that connection, the gadget blinks twice every 0.8 s for
  30 s and one physical press of its button (its talk key; a press injected
  from a console or the Nordic UART never counts) allows that update (set
  up or not, a gadget with no device token included; the
  press holds for that image, its length and SHA-256, for 10 minutes, across
  a reconnect: an upload that names no SHA-256 is refused, and so are bytes
  past that length or that don't hash to it; Forget ends it; the SAFE
  mode the button asks for, below, needs no press). Bench and automation
  builds take updates with no press, so a bench or automation run never
  waits for one; it runs on test after the next reset and keeps
  itself once it has been up and advertising for 30 seconds with its board
  ready and a central has talked to it (the token proof, or the updater
  reading its image list; an app's confirm takes effect then), or MCUboot
  swaps the old one back.
- **Recovery.** A watchdog restarts a gadget whose threads stop; after three
  boots in a row that never got healthy, it starts in a SAFE mode (Bluetooth
  and firmware updates only); on a release build that SAFE refuses a
  Bluetooth update without the token proof, set up or not (it can't ask
  for a press). Holding the button 3 to 5 s at
  power-up asks for a SAFE mode that takes updates with no press, for
  recovery by hand (a 5 to 10 s
  hold resets setup, below; in a SAFE the gadget entered by itself either
  hold gives the button's SAFE, and the reset waits for a normal boot); SWD
  through the board's probe always works. The last reset's
  reason and any crash are logged at boot and shown by the `reset` command
  of the Nordic UART console.
- **Settings** set by the app (the clip queue, the capture codec, rate and
  bitrate) survive a restart once saved: at once on a set-up gadget; on one
  that isn't set up, at most every 10 minutes, at setup, or before a
  planned power off. Push-to-talk is per connection, as the
  protocol says. The gadget lists exactly the settings it accepts.
- **Setup with the Muse app.** Until it is set up, the gadget also offers
  Muse Link setup, the same Bluetooth setup the Wi-Fi gadgets use (community
  pairing v5): the app finds it, you press the button to confirm, and the app
  sends a device token pair for its node id (`homelink-` and the end of its
  name). The XIAO advertises as `MuseGadget-XN54-XXXXXX` (XN54 is its Muse
  Gadget catalog code, XXXXXX the end of its Bluetooth address). The gadget tells the app it has no Wi-Fi, so the app skips that
  step. It stores the tokens and a proof key derived from them, then stops
  advertising setup. To set it up again without forgetting it first, put
  it in setup mode: hold the button while plugging the board in (or
  pressing its reset) and let go within 3 seconds, before the SAFE cue
  (Reset, below). Setup mode lasts 5 minutes, or until a re-setup is
  confirmed, and one press of the button confirms the re-setup, as at the
  first setup. Outside setup mode a set-up gadget refuses a new setup:
  the button is also the talk key, so nobody nearby can turn your next
  talk press into a setup. A new
  setup request on the prompt's connection takes the prompt over, and one
  that fails its checks ends the prompt, with nothing confirmed. The new
  tokens replace the old ones only once they're stored. Set your SDK token with
  `CONFIG_MG_SDK_TOKEN`: setup hands it to the app.
- **Token proof.** On every connection the app checks that the gadget holds
  the token it set up, without either side sending it (a challenge and
  response on a key derived from the token, mgcommands command 34). The app
  can also erase the gadget's token that way when you forget it.
- **Battery and power off.** The app can ask for the battery (voltage and
  charge when a cell is connected) and turn the gadget off; a press of the
  button turns it back on. With a LiPo connected, the LED flashes once every
  30 s at 10 % or less, and the gadget turns itself off at 3.3 V, before the
  cell runs flat.
- **Clock.** The app sets the gadget's clock, time zone and 12/24-hour choice
  on every connection (device actions 18/19/20); the time zone and the
  12/24-hour choice survive a restart, the time until the next connection.
  A time before 2024 is refused.
- **Its own session with your Muse.** Like a Wi-Fi gadget, the gadget is
  a device of its own on your Muse: once the app has checked its token, the
  gadget hands the app its credentials sealed under a key from that token
  (no Bluetooth pairing or bond is involved), and the app carries the
  gadget's own session to your Muse (mgcommands command 0x89). The gadget registers
  its commands there, so your Muse can check its health (`device.health`)
  or light its LED (`device.led`) directly. The app forwards the session's
  messages without reading them, and hands back refreshed tokens for the
  gadget to keep.
- **Reset.** Hold the button while plugging the board in (or pressing its
  reset), keep holding for 5 seconds until the LED flickers fast, then let
  go within 5 seconds: the gadget forgets its tokens and is ready for setup
  again. Letting go between 3 and 5 seconds asks for SAFE mode instead
  (Recovery, above); a shorter or longer hold, or holding the button to wake
  the gadget from power off, does nothing.
- **The radio link.** The gadget asks for the 2M PHY and 251-byte packets
  as soon as a phone connects. Several gadgets can share one phone, so only
  the one in use keeps a short connection interval (link modes, settings
  16 and 17): **ready** at connect (30-45 ms, latency 5, 4 s timeout),
  **active** while it's busy (15 ms, latency 0: a press, the microphone, a
  clip download, a firmware update, Link setup, or the app asking), back to
  ready 5 s after, and **sleep** (125-200 ms) after 10 minutes of nothing or
  when the app asks. All three are within Apple's rules. Watch it on the
  Nordic UART mirror (`esp32/tools/mg_ble_client.py log`) or the serial
  console: `mg.ble: phy tx 2M rx 2M`, `mg.ble: link active (talk press)`,
  `mg.ble: conn 15.00 ms latency 0 timeout 4000 ms`, `mg.ble: link ready
  (activity over)`.
- **Standard services**: Battery (read from the battery if one is connected,
  100% on USB without one), Device Information, a Nordic UART console that
  mirrors the log, and MCUmgr's SMP service.
- **Security.** The Bluetooth link isn't encrypted (no pairing or bond). The
  gadget's own Muse session is sealed end to end under keys from the token
  proof; the rest (push-to-talk audio, settings, clips) is plaintext
  musegadgets. The token proof says the gadget holds the token, not that the
  link is private. Once the gadget is set up, the microphone, clip
  downloads and the commands that erase or store something need the token
  proof on that connection (setting the clock still works without it, but
  its time zone isn't stored); on a release build a firmware update needs
  the proof or one physical press of the gadget's button (set up or not;
  an injected press never counts), the
  phone's name and app and the gadget's local time need the proof, and the
  microphone also needs a press within 5 s.

## The LED

The LED speaks the Muse Gadgets LED language for single-color lights.
Highest first:

| Pattern | Meaning |
|---|---|
| very fast blink (50 ms on, 50 off) | press the button to confirm setup with the Muse app (setting it up again too, in setup mode) |
| two short blinks every 0.8 s | press the button once to allow a firmware update (30 s) |
| on | the microphone is capturing, for the app or for an offline clip (from the moment you press) |
| three quick blinks, once | setup done, or the assistant's reply is done |
| two blinks, once | the offline clip was kept |
| four blinks, once | the press had nowhere to go (no app, no clip queue): nothing was recorded |
| three fast blinks (100 / 100), once | something went wrong |
| slow blink (150 on, 850 off) | thinking (after you let go, until the reply starts) |
| blink (500 / 500) | responding |
| one short flash every 30 s | battery at 10 % or less |
| dark | idle: connected or not, nothing to show |

Nothing ever hides the capture light but a setup confirmation. The LED is
dark when idle, on USB too: the board can't tell USB from a battery, and
`CONFIG_MG_LED_IDLE_HEARTBEAT` adds a 20 ms blink every 4 s for a bench
board that should show a sign of life. The LED isn't
dimmable (a plain GPIO), so the app gets no brightness setting.

After you let go the LED shows thinking until the app reports the reply
(responding, then done, or error); with an app that never reports it, it
shows done after 15 s. Each change is logged (`mg.ble: link …`,
`mg.face: …`, `mg.led: …`) on the console and the Nordic UART mirror. The
board has no vibration motor, so it doesn't offer the vibration settings.

## Build it

With the toolchain set up as in [`AGENTS.md`](AGENTS.md):

```sh
west build --sysbuild -b xiao_nrf54l15/nrf54l15/cpuapp path/to/muse-gadget-sdk/zephyr -d build-xiao \
  -- -DEXTRA_CONF_FILE=$HOME/.config/muse-gadgets/zephyr-sdk-token.conf -DSB_EXTRA_CONF_FILE=sysbuild-dev.conf
python3 path/to/muse-gadget-sdk/zephyr/tools/flash_hex.py build-xiao -o mg-xiao.hex
pyocd flash -t nrf54l mg-xiao.hex
```

The Muse app finishes setup only with your Muse Gadgets SDK token in the
image. Keep it in a conf file outside the repository,
`~/.config/muse-gadgets/zephyr-sdk-token.conf` (one line,
`CONFIG_MG_SDK_TOKEN="mgst_..."`, readable only by you), and pass that file
as `EXTRA_CONF_FILE`, never with `-D` on the command line: a failed configure
prints the whole command line ([`AGENTS.md`](AGENTS.md), Muse Gadgets SDK
token). The build directory and its images then hold the token: keep them
to yourself.

`--sysbuild` builds MCUboot alongside the app; the board doesn't boot
without it. Flash through the XIAO's on-board CMSIS-DAP probe (its SAMD11)
with pyOCD or OpenOCD, from the one padded hex `tools/flash_hex.py` makes
([`AGENTS.md`](AGENTS.md) has the commands).
The USB console (single-key commands: talk button, status) is in every
build. Release builds (the default) have no automation tools, and nothing
on the console confirms a setup there: only the button does. A bench build
(`overlay-bench.conf`, for the lab and CI) turns them on with one switch,
`CONFIG_MG_AUTOMATION`: its console's `>pair.confirm` (and `d`) confirm
the Muse app's setup instead of a press, for unattended tests
(`../esp32/tools/mg_confirm.py`). A gadget someone wears or carries runs a
release build, with its microphone gates on; test, bench and
automation builds go on bench units only.
In a release build the Nordic UART's state-changing words need the token
proof; the recovery tiers are in every build. Bench mode
(`CONFIG_MG_BENCH_MODE`, in `overlay-bench.conf`, or `overlay-bench-mode.conf`
on top of any build) makes every human step scriptable for a test rig; its
image is marked `.bench` and never ships.

`sysbuild-dev.conf` signs with MCUboot's development key, which anyone has,
so it needs no private key of your own. Without it the build stops unless you
give a key of your own (`-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"/path/key.pem\"`),
an option for someone shipping their own product.

## Tests

`python3 tests/host/run.py` checks Muse Link setup, the token proof and the
gadget's own Muse session on any computer with a C and a C++ compiler and
mbedTLS: the setup crypto against the ESP32's vectors, every setup step as
the app drives it, storage and its rollback, the token proof against the
protocol's vectors, and the session against the protocol's session vectors
byte for byte (its message codec also against the ESP32's own). Unit tests run
on Zephyr's `native_sim`, and end-to-end tests run two simulated radios in
[BabbleSim](https://babblesim.github.io): the gadget and a test phone that
connects, downloads offline clips, and sets the gadget
up over Muse Link and runs the token proof. Those need Linux;
[`AGENTS.md`](AGENTS.md) shows how to run them from a Mac with a container.

## License

Apache 2.0. See [`../LICENSE`](../LICENSE). The codecs in
[`../xplat`](../xplat) are Apache 2.0 from Google, with their own license files and provenance notes.
[`src/zephyr/smp.c`](src/zephyr/smp.c) is a copy of Zephyr v4.4.2's MCUmgr SMP
transport (Apache 2.0, Runtime.io's and Nordic Semiconductor's copyright, in
its header) with three changes marked in the file.
