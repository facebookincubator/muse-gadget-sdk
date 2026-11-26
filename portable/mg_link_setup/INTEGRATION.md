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

# Integrating the portable Muse Link setup core

How to put Muse Link setup v5 and the token proof into a gadget firmware
(Muse Gadget setup contract v2). Everything here is host-tested; the
"Hardware gates" section lists what only a device can prove.

## 1. What to compile

| Always | `src/mg_link_setup.c`, `src/mgls_util.c`, `src/mg_token_proof.c` |
|---|---|
| Crypto, one of | `src/crypto/mgls_crypto_builtin.c` + `src/crypto/mgls_uecc.c` (self-contained), or `src/crypto/mgls_crypto_psa.c` with `-DMGLS_CRYPTO_PSA` (an SDK that links mbedTLS / TF-PSA-Crypto) |
| Optional | `src/mgls_store.c` (the setup set on two raw flash slots) |
| Include paths | `include/`, `src/`, `src/crypto/`, and the repository's `protocols/` (for `mgcommands.h`; vendor it with the core) |
| Flags | C99; `-ffunction-sections -fdata-sections` and `--gc-sections` drop micro-ecc's unused signing code |

Defines (all default off unless noted): `MGLS_ENABLE_LOG` (1),
`MGLS_ENABLE_SIMULATED_PRESS` (bench builds only), `MGLS_AES_CT_SBOX`
(compute the AES S-box instead of a 256 B table, for parts whose table reads
are cache-timed), `MGLS_TEST_HOOKS` (host tests only), and the limits in
`include/mgls_config.h` (or a `-DMGLS_USER_CONFIG_FILE='"cfg.h"'`).

Every exported symbol starts with `mgls_`, `mgtp_`, `mglss_`, `mglsc_` or
`mgj_`, micro-ecc's included (`mgls_uECC_*`), so the core links beside a
vendor's own micro-ecc (some vendor SDKs' crypto libraries export the
plain `uECC_*` names). `tests/run.sh` fails on an unprefixed symbol. The core uses
`memcpy`, `memmove`, `memset`, `memcmp`, `strlen` and `strcmp`, no atomics,
no CSR or other privileged instructions, no floating point.

## 2. The calling contract

- **One context.** Every `mgls_*` and `mgtp_*` call runs to completion on
  the caller and none may run concurrently with another: call them all from
  the main loop (bare metal) or one task (RTOS). If the BLE stack delivers
  GATT writes in an interrupt or another thread, copy each write into a
  small queue there and call `mgls_rx()` from the loop, or take one mutex
  around every call.
- **`mgls_rx()` is cheap**: it copies the write into the workspace. The
  message is handled in **`mgls_poll()`**, which is also where P-256 runs:
  a hello costs ~27 M rv32 instructions (~0.23-0.34 s at 120 MHz), the
  other steps 0.1-2.3 M. Feed the watchdog accordingly; the BLE controller
  keeps the link through it.
- Call `mgls_poll(now_ms)` at least every ~50 ms while `mgls_idle()` is
  false, and soon after `mgls_rx()`, `mgls_press()` and a TX-complete event
  (to resume notifications that found no buffer). When `mgls_idle()` is
  true nothing is pending and the loop may sleep until the next BLE event.
  `now_ms` is any monotonic millisecond clock (wraps are fine).
- **Callbacks must not call back into the core**, with one exception:
  `disconnect()` may deliver `mgls_disconnected()` synchronously.
- `mgls_init()` once at boot. It returns `MGLS_E_CONFIG` when a required
  hook is missing: `set_up()` always, and `may_prompt()` in a release build
  (`MGLS_RELEASE`, the default). The core then fails closed: every hello
  gets `error_pairing_unavailable` and nothing shows, so stop the board's
  self-test on it. A test, bench or automation build (`MGLS_RELEASE=0`, the
  default with `MGLS_ENABLE_SIMULATED_PRESS`) may leave `may_prompt()` NULL
  or name `MGLS_HOOK_TEST_DEFAULT(set_up)` / `MGLS_HOOK_TEST_DEFAULT(may_prompt)`
  to stay hands-free; a release build can't (the names don't exist there).
  Where the config is a compile-time constant, `MGLS_STATIC_HOOKS(set_up_fn,
  may_prompt_fn)` beside it fails the build on NULL.
- `mgls_connected()` / `mgls_disconnected()` for
  every connection (the setup service is always registered, so any
  connection may use it); `mgls_mtu_changed()` after the MTU exchange.

## 3. Platform hooks (`mgls_config_t`, const, in flash)

| Hook | Contract |
|---|---|
| `bd_addr` | the identity address, most significant byte first. `node_id` (`homelink-` + the last 3 bytes, lowercase hex), `mac` (`aa:bb:..`) and `device_id` (`hatch-link:<mac>`) derive from it unless given as strings (each at most `MGLS_ID_MAX` = 32 bytes). The node id is also the proof key's HKDF info, so it must never change. |
| `firmware_version` | `device_info.version` and the transcript's `firmware_version` |
| `mgcommands` | 1: `device_info.mgcommands: 1` (the gadget serves `MG_SERVICE_UUID`). `wifi` is always `"none"` |
| `borrow(ctx, need)` / `release(ctx, ws)` | lend `need` (= `sizeof(mgls_workspace_t)`) bytes, 4-byte aligned, or NULL (the write is dropped; the phone times out). Called at the first setup write; the region comes back through `release()` already wiped (keys, tokens) once the session ends or the client disconnects (section 6) |
| `notify(ctx, data, len)` | one notification on TX, at most MTU - 3 bytes, except a bare plaintext error status (`error_encryption_required` and the three `error_pairing_*`, up to 27 bytes), which goes whole in one notification as on the ESP32 firmware; below MTU 30 the stack may cut or refuse it, and the status characteristic's read (`mgls_read_status()`) still has it whole. Copy the bytes before returning. `MGLS_OK` queued, `MGLS_AGAIN` no buffer now (the same chunk is retried at the next poll), < 0 failed (the message is dropped; a failed prompt or confirmation disconnects) |
| `disconnect(ctx)` | end the connection (the reference's delayed disconnects after errors and timeouts) |
| `random(ctx, out, len)` | a CSPRNG (hardware TRNG, or a DRBG seeded from one). Device nonces, the token proof's nonces and, on the builtin backend, P-256 key generation and Z randomization. 0 on success |
| `commit(ctx, cred)` | atomically replace the stored set: `access_token`, `refresh_token`, K (32 B), flags `MGLS_CRED_SETUP_COMPLETE | MGLS_CRED_WIFI_SKIPPED`. Return 0 only once durable; on failure leave the previous set exactly as it was. `mglss_commit()` does this (section 7). With `portable/mg_device_session`, reset every live device session (`mgds_reset(t, true)`) first: a sealed clear one has taken runs then, on the old set |
| `erase(ctx)` | erase the set (encrypted `unpair` on a confirmed session); NULL answers `error_storage` |
| `sdk_token(ctx, &len)` | the SDK token (`mgst_...`, at most `MGLS_SDK_TOKEN_MAX` = 96 chars) or NULL. Keep it out of the repository: a build-config fragment kept outside it, or written once by a provisioning tool |
| `event(ctx, ev)` | `MGLS_EV_*`, section 5 |
| `log(ctx, fmt, a, b)` | a printf format with two longs. Never carries a token or key (lengths only) |

The token proof has its own `mgtp_platform_t`: `get_k()` (K from storage:
0, `MGTP_NO_TOKEN`, or < 0), `random()`, `send()` (one Control frame,
command byte first) and `clear()` (erase the set and go back to setup
advertising; called after the result 1 went out). `get_k()` runs twice per
proof: at the challenge and at the confirm (the confirm's MAC is computed
from the pending nonces when it arrives). `mgtp_take_nonces()` hands the
matching proof's nonces to the device session's seal
(`portable/mg_device_session`) once, and wipes them: one proof keys one
sealed session, never two (a second would reuse the keystream).

## 4. GATT and advertising

Register the setup service for good, beside the musegadgets service, on the
same identity address (the apps key a gadget by the peripheral they set it
up with, and re-provisioning uses the Link service on a set-up gadget,
contract 7):

| | UUID | Properties | Security |
|---|---|---|---|
| Service | `7FDD3D1C-38EA-46CF-8B46-314ECF5F240C` | primary | |
| RX | `4D593029-28A2-4A6E-A1F0-3C2D5E8F9B01` | Write, Write Without Response | open (the records are encrypted end to end) |
| TX | `D75DC4CA-7B2B-4E9C-8F0A-1D2E3F4A5B6C` | Read, Notify + CCCD | open |

`MGLS_*_UUID_LE` give the little-endian byte lists. RX writes go to
`mgls_rx()` whole (up to MTU - 3 bytes: Android writes `negotiatedMtu - 3`);
a TX read returns `mgls_read_status()` (the last plaintext status or
`encrypted_status`, at most 31 bytes). Ask for the largest MTU the stack
allows: a provision_v2 is ~3.5 KB on the wire.

Advertising (contract 2):

- `mgls_adv_build(buf, 31, set_up)` returns the whole Link setup
  advertisement (26 bytes): Flags `06`, the complete 128-bit service UUID,
  manufacturer data `FF FF <flags>` with `mgls_adv_flags()`: `0x02` (a
  Muse Gadget over Bluetooth, not set up) or `0x03` (set up).
- Not set up: advertise it, alone or taking turns (~1.5 s each, from the
  same advertiser and address) with the gadget's musegadgets advertisement.
- Set up (`MGLS_EV_PROVISIONED`, or the store holds a set at boot):
  the musegadgets advertisement; the Link one may stay in the rotation with
  `0x03`. Back to `0x02` after a reset or the proof's `clear`.
- Scan response: the name `MuseGadget-<Model>-XXXXXX` (`mgls_format_name()`,
  the last three address bytes in uppercase hex, e.g.
  `MuseGadget-XN54-1A2B3C`, 22 chars; the `MG-` prefix is
  deferred, `MGLS_NAME_PREFIX` holds it). As a Complete Local Name element it
  takes 24 of the scan response's 31 bytes, leaving 7 (room for a 16-bit
  UUID list such as Battery, 4 bytes); the longest model that fits a legacy
  scan response is 11 chars (a 29-char name). The node id stays
  `homelink-` + the same three bytes in lowercase.

## 5. The press and the UI (R3)

- Wire every key to `mgls_press_key(s, key)`: the talk key as
  `MGLS_KEY_TALK` (`mgls_press(s)` is that), any other key (a dedicated
  confirm key) as `MGLS_KEY_OTHER`; on a buttonless gadget a deliberate,
  documented input such as a tap on a dedicated screen, never "confirm on
  connect". One press confirms: no hold, no minimum or maximum press time,
  no edge check:
  - `MGLS_PRESS_CONFIRMING`: the prompt is up; the next `mgls_poll()` sends
    `pairing_confirmed`.
  - `MGLS_PRESS_DISMISSED`: the talk key on a re-setup prompt, which it
    only dismisses (below).
  - `MGLS_PRESS_IGNORED`: a pairing waits but its prompt isn't up yet.
    Not remembered: press again.
  - Whatever is not `MGLS_PRESS_NONE` makes the press the setup's: no
    push-to-talk, menu or wake action. `MGLS_PRESS_NONE`: it is the
    firmware's.
- **Touch:** a tap on the on-screen Confirm disc (its hit area the disc, its
  ring and a margin of 10 % of the screen's size around them) calls
  `mgls_confirm_gesture()`; a tap elsewhere does nothing.
- **Re-setup of a set-up gadget:** set `set_up(ctx)` in the config (true
  while the gadget holds a set; `mglss_set_up()` with the store of section
  7). It is also true while a slot couldn't be read: a slot it couldn't
  read may hold a set, and unreadable is not empty, so the gadget is
  treated as set up until a load reads both slots. `mglss_set_up()` keeps
  that state itself (after `mglss_load()`, or the reload inside
  `mglss_commit()` or `mglss_erase()`, got -2); a board with its own store
  returns true then too. It is required:
  without it the core can't tell a set-up gadget from a new one, so
  `mgls_init()` returns `MGLS_E_CONFIG` and every hello is refused
  (`error_pairing_unavailable`). A
  re-setup is never confirmed by the talk key: the Confirm disc or another
  key confirms it, and a talk-key press while its prompt shows only
  dismisses it (`MGLS_PRESS_DISMISSED`: no talk turn), so whoever raised the
  prompt never gets the owner's next talk press. A re-setup's `provision_v2`
  `ota_url` is ignored (logged): the setup goes on without it.
- **A board whose only key is its talk key** sets `setup_mode(ctx)`: true
  in setup mode (booted with that key held, into its normal firmware, never
  SAFE) until the confirmed re-setup (`MGLS_EV_CONFIRMED`) or 5 min after
  that boot, and always on test and bench builds. Outside it a re-setup
  hello gets `error_pairing_unavailable` in plaintext and nothing shows. In
  setup mode it reports its key as `MGLS_KEY_OTHER`.
- **Dismiss:** the back gesture (a swipe, or a separate back key) calls
  `mgls_dismiss()`, for a first setup's prompt or a re-setup's: the sealed
  `pairing_confirm_timeout` and a drop from the next `mgls_poll()`, the
  stored set kept; the phone can start again. No tap dismisses.
- **The re-setup prompt's rate:** a hello on the prompt's own connection
  while it is up takes the prompt over (it stays on screen, now that
  hello's; the one press confirms it); with `mgls_conn_*()` and
  `notify_to` a write from another connection while a setup runs gets
  `error_operation_in_progress` and changes nothing. A release build sets
  `may_prompt(ctx, since_ms)`, asked for a new prompt only, with how long
  ago this connection's last re-setup prompt was dismissed
  (`MGLS_NO_PROMPT`: none); return `since_ms >= MGLS_RESETUP_PROMPT_MS`
  (10 s). False: the hello gets `error_operation_in_progress` in plaintext
  (even after `pairing_ready` on this connection) and nothing shows. A
  release build requires it (`mgls_init()`: `MGLS_E_CONFIG` without it).
  Test, bench and automation builds (`MGLS_RELEASE=0`) may leave it NULL or
  name `MGLS_HOOK_TEST_DEFAULT(may_prompt)`: every hello goes on.
- The prompt: show "Pair with Muse" with "Tap to pair" (or "Press <key> to
  pair") on `MGLS_EV_CONFIRM_ARMED` (`mgls_confirm_armed()` is true until it
  ends) and remove it on `MGLS_EV_CONFIRMED`, `MGLS_EV_CONFIRM_TIMEOUT` or
  `MGLS_EV_SESSION_ENDED`. It is up from the first `mgls_poll()` after
  `confirm_required` has gone out to the stack, and stays until the press,
  a dismiss or the phone giving up (a disconnect, a new hello): the core
  keeps no timer on it.
- `MGLS_EV_NO_SDK_TOKEN`: show or log that the gadget holds no SDK token
  (it still pairs far enough for the app to say so).
- Bench builds only, with `MGLS_ENABLE_SIMULATED_PRESS`: a console line
  `>pair.confirm` calls `mgls_simulated_press()` (it confirms what is
  armed, a re-setup included) and answers `@pair.confirm confirmed` or `@pair.confirm none`;
  print `@pair.pending` on `MGLS_EV_CONFIRM_ARMED`. `esp32/tools/mg_confirm.py`
  and `mg_ble_client.py setup --confirm-serial` then drive setup unattended.
  Never define it in a shipping profile.
- `MGLS_EV_PROVISIONED`: call `mgtp_reset()` on every live mgcommands
  connection (contract 7) and switch the advertising flags to set up.
- `MGLS_EV_UNPAIRED`: the set is gone. Call `mgtp_reset()` and
  `mgds_reset(t, true)` on every live mgcommands connection, so no matched
  proof or sealed session outlives it, and switch the advertising flags back
  to not set up, as after the proof's `clear`.

## 6. RAM

| | rv32 / Cortex-M |
|---|---|
| permanent: `mgls_t` + `mgtp_t` (+ `mglss_t`) + 12 B static | 36 + 40 (+ 16) + 12 B |
| lent workspace, default limits | 4,564 B |
| stack: `mgls_poll()` (hello, P-256) / `mgtp_rx()` / `mgls_rx()` | 960-992 / 712-800 / 40-80 B worst case, plus the deepest callback (QEMU measured 1,040 B for the hello with its callbacks) |

The workspace is borrowed at the first write to the setup service and given
back, wiped, when the session can't continue: right after a plaintext
exchange (`get_device_info`), after a timeout or error, or at disconnect. A
pairing holds it while it runs: 60 s at most for `client_finished`, then
for as long as the prompt is up, then 120 s at most for `provision_v2`. After
`auth_ok` the consumed session keeps it, to refuse a second `provision_v2`
with an encrypted `error_pairing_confirm_required` as the reference does,
until the disconnect, a new hello, or `mgls_abort()`. An mgcommands-only
connection (media, calls) never borrows it. `mgls_abort()` ends a session at
once and returns the workspace: call it before media takes the region (after
`MGLS_EV_PROVISIONED`, wait for `mgls_idle()` so `auth_ok` has gone out) and
at a factory reset.

Shrinking it: `MGLS_PROVISION_MAX` sizes the RX buffer (RX ≈ 4/3 of it +
209 B); 2,400 B still fits the measured 403 B / 1,396 B tokens with every
optional field the iOS app may add (workspace ≈ 4.35 KB). `MGLS_JSON_MAX_FIELDS` (24 × 8 B) and
`MGLS_ID_MAX` / `MGLS_SDK_TOKEN_MAX` (TX) are the other terms. Larger
messages are refused cleanly (`error_storage` on a confirmed session),
never overflowed.

**A bare-metal gadget with a media workspace** (e.g. 6 KiB, guarded): lend it
while no media runs; a media start while it is lent calls `mgls_abort()`
first (during a pairing that ends the pairing: refuse the media start
instead while `mgls_session_active()`). The ~1 KB of stack the hello needs
can come from the same region: run `mgls_poll()` on a stack carved from the
lent block (4,564 B workspace + ~1.1 KB stack = 5.7 KB < 6 KiB) if the main
stack lacks the headroom (a main loop can be down to a few hundred bytes
free).

**An RTOS gadget with no room for another task:** run every call in the
existing BLE / app task; give that task ~1.2 KB of stack headroom for
`mgls_poll()` plus the callbacks.

## 7. Persistence (R4, R6)

What the contract needs, in one commit inside the provisioning session:
both tokens, K, the setup-complete marker and `setup_wifi=skipped`; `auth_ok`
only after it; on failure `error_storage` and the old set (if any) intact;
after success the old token and K gone (never two sets); a reboot keeps it
and the proof matches after it.

`mgls_store.h` gives exactly that on raw flash:

- Two slots, each one 4 KiB sector (a record is at most 3,120 B, padded to
  3,136 B, + a 32 B commit unit: 3,168 B with the default limits; `MGLSS_RECORD_MAX` = 4,095 enforced
  at build time, so a slot also fits a key-value store entry of up to
  4,095 B).
- `mglss_flash_t`: `erase(addr, len)`, `write(addr, data, len)` (always
  32-byte units aligned to the slot, padded with 0xFF; nothing is
  programmed twice) and `read(addr, data, len)`.
- Boot: `mglss_load()` keeps the newest valid slot (commit unit "MGS2" and a
  CRC-32 over the record) and erases anything else: a torn commit, or the
  old slot a power loss kept from being erased. A read that fails erases
  nothing: it returns -2 with the state "unreadable" (`mglss_set_up()` true,
  no K or token readable: `mglss_get_k()` returns an error, never "no
  token"), and the next boot or commit reads again.
- `commit()`: `mglss_commit()` erases the other slot, writes the record,
  writes the commit unit last, reads it all back, then erases the old slot.
  With no set loaded it loads again first: a slot still unreadable fails the
  commit with nothing written (-2: it could hold a set that would win at the
  next boot). An old slot whose erase fails while its set still stands
  fails the commit too: the new set is taken back, the old one kept.
  Host-tested cut at every one of its 67 flash operations, torn or with the
  power gone: the gadget always boots with exactly one set, the old one
  unless the commit had finished.
- `erase()`, hold-to-reset and the proof's `clear`: `mglss_erase()` (the
  valid slot first). The proof's `get_k()`: `mglss_get_k()`.
  `mglss_read_token()` copies a token out for the firmware's own use.

On a key-value store instead (a vendor SDK's record store or journal): write
the whole record as one entry of at most 4,095 B, read it back with a buffer
of the full size and check the returned length (some stores' reads truncate
silently), and keep two entries plus a selector written last, or the
store's own atomic replace, so a failure leaves the old set. Never log any
of a token's characters or K: whether there is one, its length and a SHA-256
prefix at most (`protocols/README.md`).

## 8. mgcommands: the token proof

- On `mg_command_token_proof` (34) call `mgtp_rx(&proof, &frame[1], len - 1)`;
  frames go out through `send()` on Control: response `[34, 1, nonce 16,
  mac 32]`, result `[34, 3, 0|1]`, errors `[25, 34, 1, code LE16]`.
- List 34 in `supported_features`.
- `mgtp_reset()` at every connect and disconnect and at `MGLS_EV_PROVISIONED`
  and `MGLS_EV_UNPAIRED`.
- With `mg_device_session`: `mgds_reset()` at every token proof challenge
  (before `mgtp_rx()` handles it), so a session never outlives the proof that
  keyed it, and in the proof's `clear()` before it erases the set, so a
  sealed clear the session took and hasn't run yet erases the set it was
  sealed for.
- On a gadget that holds a token, gate the commands `mgcommands.h`'s Token
  proof section lists on `mgtp_matched()` for this connection: mic and camera
  output (held), `start_mic` unless a challenge is outstanding, offline clip
  reads and queue erase, power off, audio and video streams to it
  (`start_streaming`), `clear` and the session hello; on a release build
  (automation builds are exempt) also `ui_input` `hid_key`, a legacy skill
  invoke and `haptic_play`. Bluetooth
  updates over SMP need no proof: mgcommands.h's Token proof section admits
  them with a confirm press on the gadget (rc 10 while it asks, rc 11 after a
  timeout or cancel).
  Before a match answer `[25, cmd, 1, proof_required]` (code LE16). The core
  gates `clear` itself, and `mg_device_session` the hello (its port's
  `proven()`, which reads `mgtp_matched()`);
  the rest is the integration's. A match says the gadget holds the token this
  phone set up, not that the link is secure.
- `clear` after a match answers result 1, then `clear()` erases the set
  (`mglss_erase()`) and the gadget advertises setup again (flags `0x02`).

## 9. Checklist (contract v2)

| | Requirement | The core | The integration |
|---|---|---|---|
| R1 | advertising and classification | `mgls_adv_build()` / `mgls_adv_flags()` (bit 1 gadget, bit 0 set up); `device_info` `wifi:"none"`, `mgcommands:1` | advertise Link setup (0x02) until set up, rotate with the gadget advertisement from one address; 0x03 if kept after setup |
| R2 | developer mode | nothing on the device (community `pairing_auth:"none"`) | none |
| R3 | button press | `confirm_press` only; the prompt up from the first poll after `confirm_required` went out, with no timer; one press or tap confirms a first setup, any key; a re-setup never by the talk key, which dismisses it (no hold, no press time, no edge check); a tap elsewhere does nothing; `MGLS_PRESS_IGNORED` before; `mgls_dismiss()` ends it; simulated press only with the default-off define | every key to `mgls_press_key()` (the talk key `MGLS_KEY_TALK`, another `MGLS_KEY_OTHER`); `set_up()`; a tap on the Confirm disc to `mgls_confirm_gesture()`; the back gesture to `mgls_dismiss()`; the prompt on the events; never push-to-talk while pending; the bench console |
| R4 | SDK token, device token | `sdk_token` in `pairing_confirmed` from the getter (absent: no field + `MGLS_EV_NO_SDK_TOKEN`); one `commit()` of tokens + K + markers before `auth_ok`; `error_storage` on failure; tokens never logged | the SDK token source; `commit()` / `erase()` (`mgls_store.h`); the token proof handler; show "no SDK token" |
| R5 | names | `mgls_format_name()`: `MuseGadget-<Model>-XXXXXX` (`MG-` deferred) | the scan response (the name fits in 31 bytes with 7 to spare for a 22-char name) |
| R6 | re-provisioning | hello accepted while set up (a hello on the prompt's connection takes it over; another connection's gets `error_operation_in_progress` while a setup runs; on a release build a new prompt within 10 s after a dismiss on the same connection gets it too; a one-key board outside setup mode answers `error_pairing_unavailable`); confirmed by the Confirm disc or a key that isn't the talk key; `ota_url` ignored; atomic replace (old K refused, old token gone); abandoned or failed: the old set stays; one provision per session (a second gets `error_pairing_confirm_required`; a retry after a failure is taken) | keep the service registered after setup; `set_up()`; `may_prompt()` on a release build; `mgtp_reset()` on `MGLS_EV_PROVISIONED`; `setup_mode()` on a one-key board; hold-to-reset and `clear` still erase |

## 10. Hardware gates

Host tests cover the protocol, crypto and store logic. Only a device can
show: a real setup from the iOS and Android apps (with the press) and the
gadget then listed and proved as a BLE gadget; the hello's P-256 time on the
target and that the watchdog and BLE host tolerate it in `mgls_poll()`;
notification back-pressure on the vendor stack (`MGLS_AGAIN`); the store on
the real flash (and a power cut during a commit); advertising rotation and
the apps' classification (flags 0x02 / 0x03); the stack and workspace
headroom with media off and on; re-provisioning from a second phone.
