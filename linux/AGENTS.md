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
| `service.py` | `musegadget run`: reconnect loop, token rotation, local socket |
| `muse_turn.py` | Follows one request through Muse's chat events to its replies |
| `executor.py`, `fileops.py` | The commands Muse can run, as the chosen account |
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

The tests need no Bluetooth, device or network:

```sh
uv run --with pytest --with . pytest
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
- The VM accepts at most 256 KB per message from the device, so command output
  is cut at 96 KB per stream.

## Adding a command

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
  state directory, or None. It raises `ValueError` for a token that
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
  when the state directory holds no SDK token.
- It returns a `PairResult`. `PairResult.outcome` is a `PairOutcome`:
  `PAIRED`, `ALREADY_PAIRED`, `INVALID_SDK_TOKEN` or `WINDOW_CLOSED`. For
  `INVALID_SDK_TOKEN`, `PairResult.sdk_token_problem` says what is wrong.
- A paired device is left alone unless `force` is true.

### Connecting

- `Service` keeps a paired device connected to its Muse. It fetches the
  device's VMs, connects, reconnects with backoff and rotates the device token.
- `Service(identity, executor, sdk_token=None, display_name=...)` is a
  dataclass. A subclass can add its own fields.
- `Service.run()` runs until `Service.stop()` is called.
- To describe your own device, or to run your own work while it is connected,
  subclass `Service` and override any of these three methods. `Service` calls
  them for each session.
- `describe()` returns the `DeviceDescription` to register with Muse.
- `run_command(name, params, timeout_ms)` runs one command from Muse. By
  default it calls the `Executor`.
- `serve(session, vm, stop)` runs one `LinkSession` and returns its `Outcome`.
  By default it runs `session.run(stop)`, and returns `CLOSED` if the session
  fails. Override it to run your own work beside the session, and return
  when `stop` is set. The local message socket forwards to `session` while
  `serve` runs.
- `DeviceDescription(node_id, display_name, version, commands)` describes the
  device to Muse. `commands` maps each command name to its spec: a
  `description`, `required` and `optional` parameters, and an optional
  `timeout_ms`.
- `Outcome` says how a session ended: `CLOSED`, `AUTH_REJECTED`, `FORBIDDEN`,
  `UNPAIRED` or `STOPPED`.

### LinkSession

`LinkSession(noise_host=..., vm_id=..., vm_auth_token=..., device=...,
run_command=...)` is one encrypted session with a Muse VM.

- `run(stop)` connects, registers the device and serves Muse's commands until
  `stop` is set or the link ends. It returns an `Outcome`.
- `run_command(name, params, timeout_ms)` is your function. Muse calls it for
  each command and it returns the result as a dict.
- `registered` is an `asyncio.Event` that is set once Muse accepts the device.
- `send_chat(message, session_id=None, *, output_modality="text")` sends a
  user message as coming from this device. Without `session_id` it goes to
  the main chat.
- `send_voice(wav_bytes, session_id=None, *, message="", output_modality=None)`
  sends a WAV voice note of at most 2 MiB.
- `link_client.acknowledgement(result)` reads Muse's acknowledgement from the
  result of `send_chat` or `send_voice`. It raises `RequestRejected` when
  Muse refused the request.
- `open_side_chat(session_id)` looks up a side chat and returns a `SideChat`.
  `SideChat.exists` is false for an ID that Muse does not know yet. The first
  `send_chat` to that ID creates the chat.
- `subscribe_chat(session_id=None)` yields Muse's chat events.
  `chat_subscribed` is set while a subscription is open.
- `stream_tts(message_id)` yields MP3 audio for a reply that Muse gave in voice
  mode.
- A refused HTTP request raises `HttpStreamError`, which has the `status`.

### Following one request

- `muse_turn.MuseTurn` follows one request through Muse's chat events.
- Create it before you send the request. Pass every chat event to `feed()`.
  Pass the acknowledgement to `acknowledge()`.
- It returns `AgentStatus` for public progress, `Reply` each time an answer
  message changes, and `TaskFinished` when Muse's task ends.
  `TurnEvent` is the union of these three types. `AgentStatus` has
  `activity_code` and `activity_text`. `Reply` has `message_id`, `text`,
  `streamed`, `done` and `bound`, which is true when the reply is known to
  answer this request. Both texts have their control characters removed,
  except tab and newline.
- `complete(now)` says when Muse is done answering. `settled(now)` says when
  Muse finished the task and no late text arrived.
- `MuseTurn(session_id=None, *, owns_chat=False, replay_scope=None,
  ignore_parentless_until=0.0)` follows a request on the chat `session_id`.
  Set `owns_chat` when the gadget owns that chat.
- `replies` maps each answer's message ID to its latest `Reply`.
  `abandon_reply(message_id)` stops waiting for one reply.
- `acknowledge(response, follow_up=True)` binds one more user message to the
  request. Call `expect_follow_up()` before you send it.
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
