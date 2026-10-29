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

# Linux Device SDK

This turns any Linux computer, like a Raspberry Pi, into a Muse gadget.
Install it, pair it with the Muse app, and Muse can run commands and move
files on the machine.

Then make it your own: wire up a sensor, bridge a webhook, or give Muse a new
command.

> **Note:** Built by hackers, for hackers, just for fun. Proceed at your own
> risk! Muse gets the same access to the machine as the account you install it
> for.

## What you need

- **A Raspberry Pi with Bluetooth**: a 3B+, 4, 5 or Zero 2 W. Other Linux
  computers work too, as long as they have Bluetooth LE.
- **Raspberry Pi OS Bullseye or later**, Debian 11 or later, Ubuntu 22.04
  or later, Arch, or [Fedora](docs/fedora.md), already on your network.
  Debian/Ubuntu support 32-bit and 64-bit; Arch/Fedora require x86_64 or aarch64.
- **Python 3.9 or later**, using the distro's Python and Bluetooth bindings.
- **An account with sudo** on the machine.
- **An SDK token** from [gadgets.muse.ai](https://gadgets.muse.ai/settings/sdk-tokens)
  (Account > SDK tokens). Every gadget needs one to pair, including ones you
  build for yourself. Read the [Gadget SDK Terms](https://gadgets.muse.ai/sdk-terms)
  before you use it.
- **The Muse app** on your phone, to pair the device.

## Install

On the machine, as the account Muse should use:

```sh
curl -fsSL https://raw.githubusercontent.com/facebookincubator/muse-gadget-sdk/main/linux/install.sh -o install.sh
less install.sh     # read it first
bash install.sh --sdk-token mgst_…
```

The installer checks your system, installs what it needs into
`/opt/musegadget`, and starts the `musegadget` service. Before it gives Muse
your account, it asks, and tells you if that account can use sudo. Then it
opens Bluetooth pairing.

## Set it up with Muse

When the installer says pairing is open, go to the Muse app:

1. Turn on **Settings > Devices > Developer mode**.
2. Add a device (**Settings > Devices > Add Device**, the **+** icon in the
   top right). It shows up as `MuseGadgetXXXXXX`, with the name the
   installer printed.
3. Muse warns that this is a community device. Continue if it's yours.
4. When asked for Wi-Fi, pick the network shown. The machine is already
   online, so no password is needed.

That's it: the device connects to Muse and stays connected, across reboots.
Pairing is open for 10 minutes. To pair again later, run
`sudo musegadget pair`.

Every setup creates a fresh encrypted session, and pairing only opens when you
run the installer or `musegadget pair` on the machine itself. Because these are
community devices, pairing has no manufacturer verification and can't prevent
an active man-in-the-middle attack. Set it up on a network you trust.

## What Muse can do

| Command | What it does |
|---|---|
| `system.run` | Runs a shell command and returns its output and exit code |
| `file.read` | Reads a file, 64 KB at a time |
| `file.write` | Writes a file, 64 KB at a time, replacing it only when complete |
| `device.health` | Reports uptime, load, memory, disk and temperature |

Commands run as the account you installed for, with exactly that account's
permissions. If it can use sudo, so can Muse.

The service runs at most four commands at once, including commands still running
from a disconnected session. Disconnecting does not stop a command that already
started; it keeps its slot until it returns. Queued commands from the old session
are cancelled. Results from that session are not resent after reconnecting, and
the service does not automatically retry those commands.

Ask Muse things like:

> What's using all the disk space on my Pi?

> Install Home Assistant on my Pi and tell me how to open it.

> Every morning at 7, check if my Pi's backups ran and tell me if they didn't.

## Realtime voice (WAV first)

On an already paired host, run a call against an **exact leased VM ID**:

```sh
musegadget voice-call --vm-id YOUR_VM_ID --in-wav question.wav --out-wav reply.wav
```

Input is uncompressed **PCM16LE, mono, 24 kHz**. The client sends up to 960
samples every 40 ms and writes played reply audio to the output WAV. Initial
all-zero media keepalives are omitted; once playback starts, silence is kept to
preserve pauses without extending call activity. It does not resample. Input EOF
continues a quiet microphone stream (one 960-sample zero frame every 40 ms) so
automatic speech endpointing can finish; it sends neither a manual commit nor end-call.
Quiet input neither renews activity nor bypasses hold/playback gates. The client
keeps receiving until remote hang-up, Ctrl-C, or a
45-second inactivity / 300-second overall timeout. Adjust these with
`--idle-timeout` and `--overall-timeout`. The VM has one active call: a new
call can replace a phone or another device's call. These timers are client
behavior, not server-enforced limits for a paired device. Treat output WAVs as
private recordings.

Credentials come only from the existing host-local pairing store and the
normal VM lookup. `--vm-id` never changes your account's default VM and never
falls back to it. If lookup rejects an expired device token, let the normal
`musegadget run` service refresh it first. Do not copy tokens into command-line
arguments. The voice connection uses a **voice-only Noise transport**: it never
opens `/link-control` or sends `link.register`, so it cannot replace the normal
run service's live transport for the same persistent device identity. That
identity is used only for call attribution. Use `--thread-id` when targeting a
particular conversation. Every invocation admits a fresh call; resume/session-ID
targeting is not supported.

`--app-id` defaults to the protocol's `hatch-web` identity; a configured positive
signed 64-bit integer is also accepted. It is sent in the **Noise upgrade query**,
not a per-request header, and stays fixed for that connection. Other SDK sessions
keep their existing behavior. The server must admit paired devices to both
`/voice/realtime` and `/voice/end-call`; a client cannot bypass server admission.

For a microphone and speaker, install the optional extra (and the system
PortAudio library), then use `--live` instead of WAV flags:

```sh
python -m pip install 'musegadget[voice]'
musegadget voice-call --vm-id YOUR_VM_ID --live
```

Half duplex is the default: microphone input is gated during audible playback
and a short settling tail. Silent media does not mute the mic or extend idle
life. `--full-duplex` disables this echo protection; use headphones or your own
AEC. `--mic-threshold 500` optionally filters quiet microphone input by RMS level
(0 disables it; range 0–32767). After a 400 ms speech tail, filtered frames become
paced zeros, not missing audio, so speech detection still receives silence. The
provider owns turn boundaries: neither threshold muting nor EOF sends a manual
commit that could detach the final transcript from its speech-start identity.
Quiet frames do not extend inactivity. Live capture buffers only the newest 320 ms;
input captured while held/gated is discarded rather than replayed later. WAV
input pauses instead. Server input-hold events pause appends.
Speech-start and `VoiceCall.barge_in()` flush playback and fence interrupted
provider response IDs for the rest of the call. A new caller input and matching
turn admit the next reply; a bounded prefix waits for that confirmation. After
interruption, missing provider/input lineage stays muted, with no timer-based
unmute. ID or audio-buffer exhaustion ends the call rather than forgetting IDs.
`barge_in()` also best-effort cancels a known open mapper response; new speech,
not that cancellation, preempts the server's speech model. A delayed background
reply without lineage after a confirmed turn cannot be distinguished from that
turn's continuation.

The client validates HTTP success and call readiness before uploading audio.
It handles both continuous audio and legacy audio-delta records, ignores unknown
events, and never treats a cancelled response or a settled turn as call EOF.
End-call carries the client call ID, start/end timestamps and admitted thread ID,
including on cancellation and remote close. Cleanup has a deadline, and cleanup
failures never replace the original call error. A broken connection can prevent
an end-call acknowledgment; the SDK does not claim otherwise. This one-shot
voice client does not reconnect: the VM may retain the call through its grace
windows or another client's takeover until the owner ends it or a new call
replaces it.

**Limits:** response queue 2 MiB / 256 chunks, NDJSON line 1 MiB, one decoded
audio event 4 seconds, playback and unconfirmed-reply queues 5 seconds each,
upload chunk 64 KiB. Exceeding a bound fails the Linux call instead of
accumulating unbounded latency; ESP32 can instead drop an unconfirmed reply
and show REPLY MISSED. File and
protocol tests are hardware-free. Live sound-device behavior still needs testing
on your audio setup.

**Server compatibility:** this client has no channel for `ui.set.voice.end_call`.
Instead it offers `end_call_stop` (`hatch.voice.capability.offer`, right after
admission), telling Muse it ends the call on a `session_control` stop, so Muse
can hang up when you say goodbye. A VM that doesn't support that yet ignores the
offer, and a spoken goodbye doesn't hang up: use explicit hang-up or the client
timers. The SDK honors the existing
`conversation.item.added` / `session_control` / `stop` shape
on the admitted call's realtime response stream. The item's `call_id` is a
**control ID**, not the voice call ID; the dedicated stream provides the call
boundary. Stops are terminal and processed once, and cleanup ignores subsequent
records. On a stop or remote EOF the client drains queued
speech for at most 5.2 seconds, then explicitly ends the call. Without that
server support, use explicit hang-up or the client idle timer. The SDK adds no
server events or permissions itself.

### Lifecycle diagnostics

Operation-level server errors after readiness are advisory; they increment
`VoiceCall.advisory_error_count` without logging the server's message or ending
the call. Admission errors and explicit `session_dead` remain fatal. An active
`hatch.voice.turn_started` pauses the idle timer during silent asynchronous work,
including Companion work. Settlement must match a known submission ID and caller
input item; stale or unidentified settlements cannot clear newer work. A matched
settlement gives the idle timer a fresh window. The overall timeout always applies,
and tracking is bounded to 64 active and 1,024 settled submissions. Settled
identities remain for the whole call; exhaustion fails closed rather than
forgetting lineage.

`end_call_acknowledged` reports a validated end-call response. If media EOF then
stalls, a successful bounded stream reset preserves the acknowledged hangup;
`media_drain_incomplete` records that fallback. A missing end-call ACK is still a
cleanup failure.

### Streaming API

`LinkSession.stream_request()` exposes independent request and response bodies:

```python
stream = await session.stream_request("POST", "/your-stream")
async with stream:
    await stream.send_body(b'{"message":"hello"}\n')
    await stream.wait_response()  # raises HTTPStatusError on non-2xx
    # Consume response chunks now, before finishing the upload if needed.
    chunk = await stream.read()   # bytes, or None at EOF
    await stream.half_close()     # upload EOF only
    async for chunk in stream:
        consume(chunk)
```

Use `musegadget.streaming.read_ndjson(stream)` for bounded fragmented records.
Callers own their deadlines (for example `asyncio.wait_for`) and must close
abandoned streams. A slow consumer fails only its stream, not the multiplexed
control channel. `send_chat()` still returns its buffered acknowledgment.

### History and user-VM files

On servers that enable these device-token operations, the existing
`LinkSession.request()` API can use `GET /chat/history?limit=1` and
`POST /fs/read` / `POST /fs/write`. File paths follow the server's existing
home-relative rules; they are not limited to `workspace/`. No list, stat,
upload, or deletion API is implied by this access.

Send file requests with `Content-Type: application/json`. For example:

```json
{"path":"gadget-data/example.txt","offset":0,"len":4096}
```

```json
{"path":"gadget-data/example.txt","text":"hello","overwrite":false,"append":false,"create_parent":true}
```

`read` returns `data_base64`, optional UTF-8 `text`, and `eof` under the
existing `{ok,result}` response envelope. Read until `eof`, not a guessed file
size. `write` accepts exactly one of `text` or `data_base64`; the three booleans
are required. `append` supports chunked writes, but several requests are not one
atomic upload. Buffered SDK replies are limited to 1 MiB, so use modest read
lengths and history pages. Keep the acknowledgment from `send_chat()` when
correlating history entries with your request.

## Hack and extend it

Programs on the machine can send messages to Muse, with no credentials of
their own:

```sh
musegadget send-user-msg "The garage door has been open for an hour."
musegadget send-user-msg --session-id 6f1c2d4e-0b7a-4c3e-9f5d-2a8b1e0c7d93 "Posted to a side chat"
```

`--session-id` posts into a side chat: a new id starts one, and reusing it
keeps later messages there. [`examples/pebble_ring_bridge.py`](examples/pebble_ring_bridge.py)
is a complete example: a webhook listener that sends every note from a Pebble
ring to its own Muse chat.

A few other ways to build on it:

- **Let Muse do it.** Muse can run commands on the machine, so you can ask it to
  set up the rest: "Write a service that tells me when the Pi gets too hot."
- **Add a command.** Commands live in [`src/musegadget/executor.py`](src/musegadget/executor.py):
  add a spec to `COMMAND_SPECS` and a branch in `Executor.run`.
  [`AGENTS.md`](AGENTS.md) walks through it.
- **Build your own gadget.** A program on its own hardware can pair, stay
  connected and talk to Muse through the `musegadget` package.
  [`AGENTS.md`](AGENTS.md#building-a-gadget) lists what it can use.
- **Change the account.** `bash install.sh --run-as someone` gives Muse a
  different account, such as one without sudo.
- **Change the SDK token.** `bash install.sh --sdk-token mgst_…` replaces it.
  It's saved in `/var/lib/musegadget/sdk_token`, readable only by root. Muse
  records a device's token only when it pairs, so pair again afterwards with
  `sudo musegadget pair --force`. Do the same if you revoke the token the device
  paired with: it can't reach Muse until it pairs with a new one.

## Community device notes

- [RDK X5](docs/rdk-x5.md): Ubuntu ARM64 deployment notes and an optional
  community touchscreen UI, with its tested configuration and limitations.

## Manage it

```sh
sudo musegadget info                     # name, node id, SDK token and pairing state
sudo musegadget doctor                   # read-only setup and connectivity checks
sudo systemctl status musegadget         # is it running?
sudo journalctl -u musegadget -f         # follow the log
sudo musegadget pair                     # pair again
bash install.sh --uninstall              # remove it (add --purge to forget the pairing)
```

`doctor` checks saved pairing credentials, SDK token format, the systemd
service, Bluetooth controller power, and TCP connectivity to the default
Muse API on port 443. Each check prints `OK`, `FAIL`, or `WARN` and a next
step. It exits with code 1 if any check fails, or 0 otherwise. Bluetooth
problems are warnings because Bluetooth is only needed for pairing.

Run it on the gadget with sudo so it can read the root-owned state files.
It never changes settings, starts services, opens pairing, or prints tokens.
It does not validate credentials online or confirm a live Muse VM session;
an active service and reachable API alone do not establish that connection.

## Develop

From this directory, with [uv](https://docs.astral.sh/uv/):

```sh
uv run --with pytest --with . pytest
```

The tests run anywhere, with no Bluetooth or Muse needed. To try a change on a
device, copy this directory to it and run `bash install.sh --from .`.

## Community

Meet other hackers who are building and customizing Muse gadgets in our
community [Discord](https://discord.gg/3bhjCkZdd6). Get inspired, support each
other, and share what you make.

## License

Apache 2.0. See [`LICENSE`](../LICENSE).
