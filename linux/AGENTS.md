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

# AGENTS.md

How to work on this Linux client, test it, and deploy it to a device. See
`README.md` for a shorter human overview.

## What this is

A Python package (`musegadget`) that makes any Linux computer into a Muse
Home Link. It pairs with the Muse phone app over BLE, then holds an encrypted
Noise session to the user's Muse VM and runs the commands Muse sends it. It is
the Linux counterpart of the ESP32 firmware in `../esp32` and speaks the same
pairing and control protocols, so the same app pairs either.

| Module | Role |
|---|---|
| `cli.py` | `musegadget pair`, `run`, `send-user-msg`, `info`, `unpair` |
| `pair.py` | `pair()`: opens BLE setup, checks and saves the pairing, prints nothing |
| `pairing.py` | Community pairing v5: P-256 ECDH, HKDF-SHA256, AES-256-GCM, `confirm_app` |
| `ble_framing.py` | Chunked BLE framing (`0xFE`, index, total, payload) |
| `ble_setup.py` | Setup commands behind the GATT characteristics (no BlueZ dependency) |
| `ble_server.py` | BlueZ GATT peripheral and advertisement over D-Bus (GLib loop) |
| `identity.py` | Persistent identity: `homelink-xxxxxx` node id, `MuseGadgetXXXXXX` BLE name |
| `muse_api.py` | `fetch_vms` and device token refresh |
| `link_client.py` | One session: `/v1/noise` upgrade, Noise XX, `/link-control`, `/chat/stream`, `/chat/subscribe`, TTS streams |
| `streaming.py` | Bounded duplex request bodies, response chunks and NDJSON parsing |
| `voice_call.py`, `voice_audio.py` | MCU realtime voice lifecycle, WAV input/output and optional live audio |
| `service.py` | `musegadget run`: reconnect loop, token rotation, local socket |
| `muse_turn.py` | Follows one request through Muse's chat events to its replies |
| `executor.py`, `fileops.py` | The commands Muse can run, as the chosen account |
| `commands.py` | The owner's own commands, from `/etc/musegadget/commands.d/*.json` |
| `noise/` | Noise XX handshake, framing and service envelopes |
| `data/` | The systemd unit and the hash-pinned `requirements.lock`, shipped in the package |

## Prerequisites

- Python 3.9 or later. The package supports Debian 11 / Raspberry Pi OS
  Bullseye (Python 3.9, cryptography 3.3.2) and later, and Ubuntu 22.04 and
  later.
- [uv](https://docs.astral.sh/uv/) for tests and builds.
- For a real device: BlueZ, and `python3-dbus` and `python3-gi` from apt.
  These have no Linux wheels, so the venv must use the system interpreter
  with `--system-site-packages`. Don't point it at a uv-managed Python.

## Tests

Run these commands from `linux/`. The tests need no Bluetooth, device or
external network:

```sh
uv run --no-project --isolated --with pytest --with . python -m pytest -q
```

To check the oldest supported versions, pin them. cryptography 3.3.2 has no
Apple Silicon wheel, so run it under an x86-64 Python on a Mac:

```sh
uv run --no-project --isolated --python cpython-3.9-macos-x86_64 \
  --with pytest --with cryptography==3.3.2 --with websockets==13.1 \
  env PYTHONPATH=src python -m pytest -q
```

The pairing tests check against the published fixtures in
`tests/vectors/link_pairing_v5.json`. `tests/test_link_client.py` runs the
client against an in-process fake VM that does a real Noise handshake.

## Deploy to a device

Copy this directory to the device and install from it:

```sh
bash install.sh --from .            # asks before granting access
bash install.sh --from . --yes      # doesn't ask
```

Reinstalling keeps the pairing. It restarts the service, which kills any
command Muse is running at that moment, so check the log for recent `invoke`
lines first.

For a quicker loop, build a wheel and install it into the existing venv:

```sh
uv build --wheel
# on the device:
sudo /opt/musegadget/bin/uv pip install --python /opt/musegadget/venv/bin/python --no-deps --reinstall musegadget-*.whl
sudo systemctl restart musegadget
```

Installer flags: `--sdk-token TOKEN`, `--run-as USER`, `--no-pair`, `--yes`,
`--from SOURCE`, `--uninstall`, `--purge`. The installer is ShellCheck-clean;
keep it that way.

## Run and debug

```sh
sudo journalctl -u musegadget -f     # service log
sudo musegadget -v pair              # pairing, verbose
sudo musegadget info                 # identity and pairing state
```

State lives in `/var/lib/musegadget` (mode 0700): `identity.json` survives
unpairing, `pairing.json` holds the device tokens. The local socket is
`/run/musegadget/musegadget.sock`, owned by root and the run-as account's group.

A healthy start logs `commands run as <user>`, `Noise session established`,
`sent link.register` and `registered with the Muse`.
Each command the Muse runs logs `invoke <command>`, then how it ended, such as
`system.run ok, exit 0 in 41 ms` or `file.read failed in 3 ms`. The log
never has a command's parameters, output or error message.

## Pairing

- Community mode only: `pairing_auth: "none"`, epoch 0, policy `confirm_app`.
  A Pi has no button, so the app's own confirmation stands in for it, and BLE
  only advertises while `musegadget pair` runs.
- The BLE name is `MuseGadget` plus the last six hex digits of the identity,
  with **no hyphen**: the apps compare the text after the prefix with the text
  after `homelink-` in the node id. `get_device_info` must report
  `model: "hatch_link"`.
- The apps always scan Wi-Fi and send `provision_v2` with an SSID. When the
  device is online it offers one open network, "the current connection", and
  ignores the Wi-Fi fields it gets back. It never stores them.
- `provision_v2` carries `api_url`, which only older firmware reads (it adds
  `/hatch/`). This client ignores it and uses `api_url_v2` when newer apps
  send it, or `https://api.muse.ai`, with bare API paths either way.
- The Android app writes `negotiatedMtu - 3` bytes with no cap, and Android
  rejects writes over 512 bytes. BlueZ must have `[GATT] ExchangeMTU = 256` in
  `/etc/bluetooth/main.conf`; the installer sets it. Symptom if it's missing:
  pairing works up to Wi-Fi, then "Couldn't connect" in the app.
- Answer plaintext `get_device_info` at any time. Android re-sends it when it
  restarts a handshake on the same connection.
- GATT status 133 on the phone is usually stale Bluetooth state on the phone.
  Toggling the phone's Bluetooth clears it.

## Talking to the Muse

- Connect to `wss://<noise_host>/v1/noise?vm_id=<vm_id>` with the per-VM bearer
  from `fetch_vms` in an `Authorization` header. A 401 or 403 on the upgrade
  means fetch fresh VM credentials, not retry the same bearer.
- After the Noise handshake, open `POST /link-control` and leave the body open.
  Both directions carry JSON messages, each prefixed with a little-endian u32
  length. The device sends `link.register`, then a `link.result` for each
  `link.invoke`. `link.unpaired` means the Muse removed the device.
- Register as `platform: "linux"`, `device_family: "homehub"`. Never use family
  `link` or advertise `device.ota`: the server pushes ESP32 firmware updates to
  every `link` device.
- Messages from the device to the Muse (`musegadget send-user-msg`) go as separate
  `POST /chat/stream` requests on the same session, with `device_id` set to the
  node id and `"output_modality": "text"`. `session_id` picks the chat;
  `chat_id` is not an API field and is ignored. The response is only the ack
  (`message_id`); the reply appears in the Muse chat. Replies are text: to
  speak them, use a text-to-speech API of your choice.
- The VM accepts at most 256 KB per `/link-control` message, so command output
  is cut at 96 KB per stream.

## Realtime voice

- `musegadget voice-call` uses the paired store and exact `--vm-id`; never
  add an owner-token fallback or change the account's default VM. Its
  `LinkSession(register_device=False)` waits for transport readiness without
  `/link-control` or `link.register`, preserving the normal run service's live
  transport. Reuse the persistent identity for attribution only.
- Each voice invocation admits a fresh call. Omit the realtime `session_id`
  query (it is for an already-admitted call); optional `thread_id` belongs in
  the admission record. Do not imply that this client implements resumption.
- Set the configured voice app ID on the Noise upgrade query. Per-request
  app-ID headers are not a substitute. Keep ordinary chat ACKs buffered.
- Admit only `mcu-wearable` audio calls. Wait for HTTP success and matching
  call/session readiness before microphone upload. Explicitly end the call
  before upload EOS, including failure/cancellation paths, with bounded cleanup.
- PCM is signed 16-bit LE, mono 24 kHz, up to 960 samples per 40 ms upload.
  Bound response chunks, lines, decoded audio and playback. Silence keepalives
  are not activity and must not hold the mic gate shut.
- Half duplex is the default, with playback flush on barge-in and server hold
  respected. Response completion/cancellation and turn settlement are not call
  termination or proof that all PCM has arrived.
- Right after the admission record, the client sends
  `{"type":"hatch.voice.capability.offer","end_call_stop":1}`: it ends the
  call on a `session_control/stop`, so Muse may hang up that way when the
  caller says goodbye. A VM that doesn't know the key ignores it. The stop
  arrives on this call's realtime response stream. Its `item.call_id` is a
  control ID, not the admission call ID. Handle it once, drain queued playback
  within a bound, then explicitly end the call. Don't add other capabilities,
  invent server events or widen the device's permissions.
- Post-readiness operation errors are advisory unless the peer explicitly
  reports `session_dead`; admission errors remain fatal. Never log error text.
- Idle suspension follows identified active turns, including delayed aliases.
  Only a matching settlement retires that input item's work through the matched
  submission, not newer aliases or unrelated inputs. Keep identity storage
  bounded, restart idle after settlement, and never suspend the overall timer.
- A validated end-call ACK is not undone by a delayed media EOF if a bounded
  stream reset succeeds; report the incomplete drain separately.
- Tests use self-authored tones/zeros and synthetic identities only. Never put
  real recordings, credentials, internal documents or captured events here.
  `test_streaming.py`, `test_voice_call.py` and `test_voice_cli.py` exercise real
  Noise against an in-process fake VM without hardware or external calls.

## Adding a command

A device's owner adds commands without changing the package: one JSON file
per command in `/etc/musegadget/commands.d/` (`MUSEGADGET_COMMANDS_DIR`
overrides it), read when the service starts. `commands.py` documents the
format and validates it, including the directory trust check;
`Executor.run_drop_in` runs them. `Executor.specs` takes the actual device
metadata to admit whole definitions under the serialized registration budget. See "Add your own
commands" in `README.md`. To add a built-in command to the package itself:

1. Add a spec to `COMMAND_SPECS` in `executor.py`: `description`, `required`
   and `optional` parameters (each with `type` and `description`), and
   `timeout_ms` if the default 30 seconds is too short.
2. Handle it in `Executor.run`. Return `ok(payload)` or `error(message)`.
3. Anything that touches the machine should run in a child process with
   `self._child_options()`, so it runs as the chosen account and not as root.
4. Add a test in `tests/test_executor.py`.

Muse sees the new command after the service restarts and re-registers.

## Building a gadget

A gadget is a program that uses `musegadget` to talk to Muse from its own
hardware, such as a robot or a speaker. It may use the parts of the package
listed here. Everything else is internal and can change.

### Identity and state

- `identity.load_or_create()` returns the device `Identity`. It creates one on
  first use and keeps it across unpairing.
- `Identity.node_id` is the `homelink-xxxxxx` ID that Muse knows the device by.
  `Identity.ble_name` is the name the Muse app shows during pairing.
- `config.state_dir()` is the directory for device state. Set
  `MUSEGADGET_STATE_DIR` to move it.
- `config.load_json(name)`, `config.save_json(name, data)` and
  `config.delete_json(name)` read, write and remove one JSON file in the state
  directory. `save_json` writes atomically and makes the file readable only by
  its owner.
- `config.sdk_token()` returns the SDK token from `MUSEGADGET_SDK_TOKEN` or the
  state directory, or `None`. It raises `ValueError` for a token that
  gadgets.muse.ai could not have issued.
- `config.PAIRING_FILE` is the file that holds the pairing.
- `config.STATE_DIR_ENV` and `config.SDK_TOKEN_ENV` hold the names of the two
  environment variables, `MUSEGADGET_STATE_DIR` and `MUSEGADGET_SDK_TOKEN`.

### Pairing

- `pair.pair(on_open=..., force=False, timeout_s=600)` opens Bluetooth setup
  so that the Muse app can pair the device. It prints nothing.
- Just before setup opens, it calls `on_open` with a `SetupWindow`. Use it to
  tell the user what to do. `ble_name` is the device to choose in the Muse
  app, `timeout_s` is how long setup stays open, and `has_sdk_token` is false
  when neither the environment nor the state directory provides an SDK token.
- It returns a `PairResult`. `PairResult.outcome` is a `PairOutcome`:
  `PAIRED`, `ALREADY_PAIRED`, `INVALID_SDK_TOKEN` or `WINDOW_CLOSED`. For
  `INVALID_SDK_TOKEN`, `PairResult.sdk_token_problem` says what is wrong.
- A paired device is left alone unless `force` is true.

### Connecting

- `service.Service` keeps a paired device connected to its Muse. It fetches the
  device's VMs, connects, reconnects with backoff and rotates the device token.
- `Service(identity, executor, sdk_token=None, display_name=...)` is a
  dataclass. A dataclass subclass can add fields with defaults. `executor`
  is an `executor.Executor` instance.
- `await Service.run()` runs until `Service.stop()` is called or `serve()`
  returns `Outcome.STOPPED`.
- To describe your own device, or to run your own work while it is connected,
  subclass `Service` and override any of these three methods.
- `describe()` returns the `DeviceDescription` to register with Muse.
  `Service` calls it for each new session.
- `run_command(name, params, timeout_ms=None)` handles each command from Muse
  synchronously in a worker thread. Commands can run concurrently. By default
  it calls the `Executor`. Return `{"ok": True, "payload": {...}}` on success
  or `{"ok": False, "error": "..."}` on failure.
- `async serve(session, vm, stop)` runs each new `LinkSession` and returns its
  `Outcome`. `vm` holds the selected VM's credentials, and `stop` is the
  service's `asyncio.Event`. The default awaits `session.run(stop)`.
  An override must run the session, finish when it ends or `stop` is set,
  and clean up its background tasks before returning the outcome. `Service`
  logs exceptions from `serve()` and reconnects with backoff. Cancellation
  propagates to the caller.
- `await Service.serve_local(path)` starts the optional local message socket
  at a `pathlib.Path`. It returns an `asyncio.Server` that the caller must
  close. The socket forwards to the registered session while `serve()` runs.
  `Service.run()` does not start this socket itself.
- `link_client.DeviceDescription(node_id, display_name, version, commands)` describes the
  device to Muse. `commands` maps each command name to its spec: a
  `description`, `required` and `optional` parameters, and an optional
  `timeout_ms`.
- `link_client.Outcome` says how a session ended: `CLOSED`, `AUTH_REJECTED`, `FORBIDDEN`,
  `UNPAIRED` or `STOPPED`.

### LinkSession

`link_client.LinkSession(noise_host=..., vm_id=..., vm_auth_token=..., device=...,
run_command=...)` is one encrypted session with a Muse VM.

- `await run(stop)` connects, registers the device and serves Muse's commands until
  `stop` is set or the link ends. It returns an `Outcome`.
- `run_command(name, params, timeout_ms)` is the synchronous callback, with
  the same threading and result contract as `Service.run_command`.
- `registered` is an `asyncio.Event` that is set once Muse accepts the device.
  It clears when the session ends.
- `send_chat(message, session_id=None, *, output_modality="text")` sends a
  user message as coming from this device. Without `session_id` it goes to
  the main chat.
- `send_voice(wav_bytes, session_id=None, *, message="", output_modality=None)`
  sends a WAV voice note of at most 2 MiB.
- `send_image(image_bytes, mime_type, session_id=None, *, message="",
  filename=None, output_modality="text")` sends a picture of at most
  `link_client.MAX_IMAGE_BYTES` (2 MiB), with `message` as its text.
  `mime_type` is one of `link_client.IMAGE_MIME_TYPES`: JPEG, PNG, GIF or
  WebP. Anything else raises `ValueError`.
- `link_client.acknowledgement(result)` reads Muse's acknowledgement from the
  result of `send_chat`, `send_voice` or `send_image`. It raises
  `RequestRejected`, with a `status` attribute, when Muse refused the request,
  or `ValueError` when the response is not a dict.
- `await open_side_chat(session_id)` looks up a side chat and returns a `SideChat`.
  `SideChat.exists` is false for an ID that Muse does not know yet. The first
  `send_chat` to that ID creates the chat. The lookup closes its subscription
  without reading events.
- `subscribe_chat(session_id=None)` yields Muse's chat events.
  `chat_subscribed` is set while a subscription is open.
- `stream_tts(message_id)` yields MP3 audio for a reply that Muse gave in voice
  mode.
- `send_chat`, `send_voice` and `send_image` are coroutines that return a
  dict with `ok`, `status` and `response`. `subscribe_chat` and `stream_tts`
  are async iterators.
- `open_side_chat`, `subscribe_chat` and `stream_tts` raise `HttpStreamError`
  on refused HTTP requests. It has `status` and `path` attributes.

### Following one request

- `muse_turn.MuseTurn` follows one request through Muse's chat events.
- Create it before you send the request. Pass every chat event to `feed()`.
  Pass `acknowledgement(result)` to `acknowledge()`.
- Both methods return lists of events. `acknowledge()` releases events held
  before the acknowledgement arrived. Each event is an `AgentStatus` for
  public progress, a `Reply` when an answer message changes, or a `TaskFinished`
  when Muse's task ends.
  `TurnEvent` is the union of these three types. `AgentStatus` has
  `activity_code` and `activity_text`. `Reply` has `message_id`, `text`,
  `streamed`, `done` and `bound`, which is true when the reply is known to
  answer this request. Both texts have their control characters removed,
  except tab and newline.
- `complete(now)` says when Muse is done answering. `settled(now)` says when
  Muse finished the task, every reply is done and the grace period passed.
  Pass a `time.monotonic()` value as `now`.
- `MuseTurn(session_id=None, *, owns_chat=False, replay_scope=None,
  ignore_parentless_until=0.0)` follows a request on the chat `session_id`.
  Set `owns_chat` when the gadget owns that chat. Its acknowledgement must
  then confirm the same `session_id` and `is_thread: true`.
- `replies` maps each answer's message ID to its latest `Reply`.
  `abandon_reply(message_id)` stops waiting for one reply.
- `acknowledge(response, follow_up=True)` binds one more user message to the
  request. Call `expect_follow_up()` before you send it. Until that
  acknowledgement arrives, the turn is neither complete nor settled.
- `acknowledged`, `busy`, `task_finished`, `activity_code` and
  `last_activity` describe the request's state.
- `ReplayScope` drops events from earlier turns. Share one across the turns
  of a chat the gadget owns. `retire()` on a finished turn adds its IDs.
  `ReplayScope.retire_request(response)` retires a request that no turn
  follows. `ReplayScope.observe_idle(event, session_id)` retires an event
  seen while no turn is open.

## Say Muse, never Hatch

Users never see the name Hatch.

- Anything a person reads says Muse, the Muse app, or the Muse's name:
  - CLI output and help
  - log lines
  - errors
  - docs
- Don't use `hatch` in a new file name or identifier. Use `muse` or
  `musegadget`. The device API client is `muse_api.py`.
- `hatch` stays only where the server or the Muse app depends on it. Don't
  rename these:
  - the host `hatch.metaaivm.com`
  - the `hatch_refresh:` auth prefix
  - pairing labels and ids such as `hatch-link-pairing-v5`, the `hatch_link`
    model and the `hatch-link:` device id, and the test vectors

## Before you hand back work

1. The tests pass, on the newest Python and on Python 3.9 with cryptography
   3.3.2 if you touched pairing or the Noise code.
2. If you changed `install.sh`, it passes ShellCheck.
3. If you deployed, the log shows `registered with the Muse` and no tracebacks.
