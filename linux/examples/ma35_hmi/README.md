# MA35 Linux HMI gateway: live chat, voice and device controls

This example connects a Nuvoton MA35 Linux HMI to Muse through a Linux laptop or
other Linux gateway. The gateway pairs over BLE and holds the encrypted Muse
session. SSH over Ethernet carries framebuffer updates, touch events and ALSA
audio to the board. Python and the speech model run on the gateway, so compact
board images do not need a Python runtime.

Validated on a NuMaker-HMI-MA35H0 compact Linux 5.10.140 image with a
1024×600 BGRA framebuffer, resistive ADC touch and a NAU8822-family codec.
MA35D1 Linux boards can use the same approach after adapting their framebuffer,
touch orientation and mixer routes. This is a hardware example, not a universal
MA35 driver or a claim that every MA35D1 configuration was tested.

## Features

- Live user/assistant text with `/chat/subscribe` deltas and replay deduplication.
- Tap to start recording; tap again to send a voice note (30-second maximum).
- Stereo capture, stronger-channel selection, exact mono WAV sizes and a mic meter.
- Local Piper TTS: complete sentences play as text arrives; recording pauses TTS.
- On-screen headphone volume/mute and a temperature dial controlled by Muse.
- Thermostat simulation reports room temperature and Heating/Cooling/Idle state.
  No physical sensor or HVAC equipment is connected. Headphone mixer control is real.

## Setup

Run these commands from this directory on the gateway:

```sh
python3 -m venv --system-site-packages venv
venv/bin/pip install -r requirements.txt
# For BLE pairing, install the distro's BlueZ, python3-dbus and python3-gi.
# Compile for the board architecture; musl/static toolchains are also suitable.
aarch64-linux-gnu-gcc -O2 -static touch_reader.c -o touch_reader
export MUSEGADGET_STATE_DIR="$PWD/state"
export MUSEGADGET_SOCKET="$PWD/musegadget.sock"
export MA35_SSH_TARGET=root@192.168.137.2
# Supply MUSEGADGET_SDK_TOKEN privately if required by your Muse account.
venv/bin/musegadget pair
venv/bin/python board_bridge.py --check
venv/bin/python board_bridge.py
```

Establish SSH key access and verify the board's host key beforehand. SSH runs
noninteractively and uses the usual OpenSSH config, keys and known_hosts. Choose
an account with the necessary framebuffer, input and audio device permissions.
The example exposes `system.run` on that board account; use a dedicated test
board and restrict advertised commands before product deployment.

For a WSL relay, set `MA35_SSH_TARGET` to the relay endpoint, `MA35_SSH_PORT`
to its forwarded port and optionally `MA35_SSH_HOST_ALIAS` to the board's host
identity. Configure Internet routing and DNS independently; the example does
not modify Windows ICS or flash NAND/eMMC/SD storage.

Place a Piper ONNX voice and matching `.onnx.json` configuration at
`voices/en_US-lessac-medium.onnx`, or set `MA35_TTS_MODEL` to another voice path.
The validated voice is [en_US lessac medium](https://huggingface.co/rhasspy/piper-voices/tree/main/en/en_US/lessac/medium):
63,201,294 bytes (63.2 MB / 60.27 MiB) plus 4,885 bytes of configuration.
Downloads are optional installation steps; synthesized audio stays local until
sent to the board. Voice notes go to Muse. Model files are excluded from Git.
Piper and each voice have their own license terms; review them before distribution.

Install DejaVu Sans at `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`.
A simple generated Muse badge and robot are included in the renderer. Optional
user-supplied `muse-logo.svg` (requires CairoSVG) and `jollybot.gif` replace them;
third-party artwork is not bundled. Copy and customize
`muse-ma35.service.example` for a persistent gateway service.

## Temperature and headphone control examples

Muse registers `display.controls` through the **Gadget SDK command API**, not a
separate MCP server. All parameters are optional; `{}` reads the current state.
Unknown parameters and out-of-range values are rejected before updates.

| Ask Muse | Command parameters |
|---|---|
| “Set the thermostat to 74 degrees and heat.” | `{"temperature_f":74,"mode":"Heat"}` |
| “Lower my headphones to 50 percent.” | `{"volume_percent":50}` |
| “Mute my headphones.” | `{"muted":true}` |
| “Unmute and set volume to 80 percent.” | `{"volume_percent":80,"muted":false}` |
| “What are my temperature and volume settings?” | `{}` |

A combined invocation:

```json
{"temperature_f":74,"mode":"Heat","volume_percent":50,"muted":false}
```

Temperature targets are integers 50–90°F; modes are Off, Heat, Cool or Auto.
Volume is an integer 0–100; mute is a boolean. Omitted settings stay unchanged.
Setting a volume unmutes unless `muted` is explicitly supplied. The response
includes target, mode, requested volume, mute, simulated room temperature and
HVAC state. ALSA has discrete mixer steps: the validated 50% request read back
as 51% at the codec. The screen and Muse responses show the requested percentage.

For a local integration check, send one JSON line to `MUSEGADGET_SOCKET`:

```json
{"action":"controls","params":{"temperature_f":74,"mode":"Heat","volume_percent":50}}
```

Other local actions are `voice.toggle`, `diagnostics` and `output.test`.
The last one is explicitly a local display/headphone check, not a Muse reply.

## Board bring-up observations

The validated compact image enabled the display DT node but omitted its
`dcultrafb` module. Installing the matching vendor `dcultrafb.ko` and loading it
created `/dev/fb0`; persist it in the image's module-loading configuration.
Use a module built for your exact kernel; no vendor binary is shipped here.
Visible frames use 4096-byte rows (1024×600×4), rotated 180°. Touch keeps raw X
and reverses raw Y because the ADC X axis already opposes framebuffer X.
Adapt these assumptions for other panels or board revisions.

Capture required PGA gain and boost, then stereo channel selection: the
validated right channel had about 103 times the left channel's RMS amplitude.
The sample uses PGA 100%, ADC 100% and PGA Boost 100%; tune gain for your mic
and environment to avoid clipping. Physical mic/channel wiring differs by board.
Set `MA35_VOICE_DIAGNOSTIC_PATH` only when you explicitly want a private retained
WAV for diagnosis; by default no recording is written to disk.

TTS preserves stdin on file descriptor 3 before backgrounding `aplay`, waits
for playback and propagates its exit status. Without preserving stdin, the
remote background process could receive no WAV data while a trailing cleanup
command returned success. The validated fix produced audible playback.

## Tests and protocol compatibility

```sh
venv/bin/python -m pytest -q .
# Existing Linux SDK regression suite, from linux/:
python -m pytest -q tests
```

The example bundles a subscription-capable `live_link.py` derived from
[PR #100](https://github.com/facebookincubator/muse-gadget-sdk/pull/100), with
protocol tests. It replaces the session implementation only within this example
process and does not change the SDK's default service or Noise implementation.
Keep this compatibility copy synchronized when SDK session internals change.
Hardware tests verified streaming replies, touch volume, voice-note acceptance,
Muse invocation of temperature/volume controls and audible TTS. Unit tests need
no board or Bluetooth; protocol tests use local in-process sockets.
Credentials, pairing state, audio, logs, compiled helpers and voice models are
excluded from version control.
