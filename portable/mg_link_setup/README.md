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

# Portable Muse Link setup (device side)

The device side of Muse Link setup v5 and the mgcommands token proof, per
the Muse Gadget setup contract v2, in dependency-free C99 for gadget SDKs
that have no Link setup of their own: a bare-metal main loop or an RTOS
task.
No allocation, no threads, no OS calls. `INTEGRATION.md` is the porting
guide.

| Path | What |
|---|---|
| `include/mg_link_setup.h` | the setup core: the 0xFE framing, community pairing v5, get_device_info, the press, token-only `provision_v2`, advertising helpers |
| `include/mg_token_proof.h` | `mg_command_token_proof` (34) for an mgcommands handler |
| `include/mgls_store.h` | optional: the setup set on two raw flash slots (atomic replace) |
| `include/mgls_crypto.h` | the crypto backend interface |
| `include/mgls_config.h` | compile-time limits (they size the lent workspace) |
| `src/` | the core, the proof, the store, the JSON / base64url helpers |
| `src/crypto/mgls_crypto_builtin.c`, `mgls_uecc.c` | self-contained crypto: SHA-256, HMAC, HKDF, AES-256-GCM, P-256 (micro-ecc) |
| `src/crypto/mgls_crypto_psa.c` | the same on PSA Crypto (`-DMGLS_CRYPTO_PSA`) |
| `third_party/micro-ecc/` | micro-ecc, BSD-2-Clause, unmodified (its README has the provenance) |
| `tests/` | host tests (`tests/run.sh`) |
| `tools/` | code size and stack per target (`size.sh`), rv32 instruction counts under QEMU (`qemu/run.sh`) |

## API in one screen

```c
static mgls_t setup;                 /* 36 B on 32-bit: all the permanent state */
static const mgls_config_t cfg = {   /* in flash: identity + callbacks */
	.bd_addr = addr, .firmware_version = "1.2.3", .mgcommands = 1,
	.borrow = lend_ws, .release = return_ws,          /* the workspace */
	.notify = tx_notify, .disconnect = drop_link, .random = trng,
	.commit = store_commit, .erase = store_erase,     /* atomic replace */
	.sdk_token = sdk_token, .event = on_event, .log = log_line,
	.set_up = holds_a_set,       /* required: without it mgls_init() refuses the config */
	.may_prompt = prompt_gap_ok, /* required in release builds: since_ms >= MGLS_RESETUP_PROMPT_MS */
	.setup_mode = in_setup_mode, /* a one-key board only: booted with its key held, for 5 min */
};
MGLS_STATIC_HOOKS(holds_a_set, prompt_gap_ok); /* beside a constant config: a NULL hook doesn't build */

if (mgls_init(&setup, &cfg) != MGLS_OK) {}     /* boot; MGLS_E_CONFIG (no set_up, or no may_prompt in a
                                                * release build): every hello refused, stop the self-test */
mgls_connected(&setup, now_ms);                /* any connection */
mgls_mtu_changed(&setup, mtu);
mgls_rx(&setup, data, len);                    /* RX characteristic write */
mgls_poll(&setup, now_ms);                     /* main loop, <= 50 ms apart while !mgls_idle() */
if (mgls_press_key(&setup, MGLS_KEY_TALK) != MGLS_PRESS_NONE) {} /* a key: not push-to-talk then */
mgls_press_key(&setup, MGLS_KEY_OTHER);        /* a key that isn't the talk key (a re-setup's confirm) */
mgls_confirm_gesture(&setup);                  /* a tap on the Confirm disc */
mgls_dismiss(&setup);                          /* "Not now": the back gesture */
mgls_confirm_armed(&setup);                    /* the UI shows "Tap to pair" / "Press <key> to pair" */
mgls_disconnected(&setup);

static mgtp_t proof;                           /* 40 B, per connection */
mgtp_rx(&proof, payload_after_cmd_34, len);    /* mgcommands command 34 */
mgtp_reset(&proof);                            /* connect, disconnect, MGLS_EV_PROVISIONED */
```

## Numbers

Default limits (a `provision_v2` of up to 2,560 B with every optional field
the iOS app sends; tokens of up to 1,024 B and 2,048 B, the pair with
node_id and the longest `access_expires_at` within the device session's
2,048 B credentials; the real ones
measured 403 B and 1,396 B).

**RAM**

| | Bytes |
|---|---|
| `mgls_t` (permanent) | 36 on rv32 / Cortex-M |
| `mgtp_t` (permanent, or per connection) | 40 |
| `mglss_t` (optional store) | 16 |
| static data (the RNG hook) | 12 |
| workspace, lent only while the setup service is in use | 4,564 (RX 3,624, TX 572, JSON fields 192, session 96, identity 64, reassembly and transmission state 16) |
| stack, `mgls_poll()` handling a hello (P-256) | 960-992 worst case (GCC call graph); 1,040 measured under QEMU with the callbacks |
| stack, `mgls_poll()` other steps / `mgtp_rx()` | 880 measured / 712-800 worst case (672 measured) |
| stack, `mgls_rx()` / `mgls_press()` | 40-80 / 16 |

**Code** (`-Os`, `--gc-sections`, the core + token proof + builtin crypto,
without the optional store; libc's mem*/str* not counted):

| Target | Logs on | Logs off (`MGLS_ENABLE_LOG=0`) |
|---|---|---|
| rv32imc, xPack GCC 15.2 | 21,538 B | 20,140 B |
| Cortex-M4 thumb, Zephyr SDK GCC 14.3 | 20,637 B | 18,570 B |

What dominates (rv32imc, logs off): the protocol core 8.2 KB (handshake,
records, dispatch, provisioning, framing; 1.4 KB more with logs), micro-ecc
P-256 4.6 KB, SHA-256 / HMAC / HKDF / AES-256-GCM 3.7 KB, the strict JSON
reader/writer and base64url 3.1 KB, the token proof 0.5 KB. The optional
two-slot store adds 1.3-1.6 KB. `tools/size.sh` reproduces these.

**Time** (`tools/qemu/run.sh`: rv32imc instructions retired under QEMU
`-icount`): a hello costs 27.0 M instructions (P-256 key generation 13.2 M,
the point check and shared secret 13.2 M, the transcript and key schedule
0.5 M); `client_finished` 0.19 M; the press 0.14 M; decrypting and
committing a 2.0 KB `provision_v2` 2.0 M (AES-256-GCM is ~790
instructions/byte: bit-serial GHASH and a byte-wise AES, small rather than
fast); a token proof challenge 0.18 M. At ~120 MHz and 1-1.5 cycles per
instruction a hello blocks the calling loop for roughly 0.23-0.34 s. On the
host (Apple M-series) the same P-256 code takes 0.14 ms per key generation
or agreement with 64-bit words, 0.36 ms with the 32-bit word path rv32
uses.

## Tests

```sh
tests/run.sh          # strict -Werror build, unit tests in four builds,
                      # end-to-end (ASan + UBSan, and on PSA)
```

- `tests/test_unit.c`: SHA-256 / HMAC / HKDF / AES-256-GCM KATs, P-256
  against `esp32/tests/vectors/link_pairing_v5.json` and invalid points, the
  token proof against
  `protocols/test-vectors/mg-token-proof-v1.json` frame by frame and its
  rules, the JSON reader and writer, base64url, advertising helpers, and the
  two-slot store cut at every flash operation (torn write, power loss).
  Built five ways: -O2, ASan + UBSan, the sealed clear (`MGDS_CAP_SEALED_CLEAR=1`), the constant-time S-box, PSA.
- `tests/test_e2e.py` + `tests/phone.py`: an independent phone (Python,
  `cryptography`) against `tests/sim.c` (the core, the proof and the store on
  a RAM flash): the vector session byte for byte; whole setups at MTU 23,
  185, 247 and 498; a prompt with no timer and the confirmed-idle timeout;
  presses before the prompt is up (the poll after it went out) swallowed,
  one press in its very millisecond confirming, an answer sent before that
  poll keeping the prompt; the back gesture dismissing it; the talk key
  dismissing a re-setup prompt and a one-key board refusing a re-setup
  outside setup mode; a hello taking its own connection's prompt over and
  another connection's getting busy; re-provisioning a
  set-up gadget (old K refused, new K matches, the old token gone, never two
  sets), confirmed by another key or the Confirm disc (a re-setup's
  `ota_url` ignored and logged);
  commit failures at every
  flash operation (the old set stays, `error_storage`); an oversized
  `provision_v2`; replayed, skipped, tampered and foreign records (session
  reset and disconnect); every error status; no / overlong SDK token; the
  longest identity strings; notification back-pressure; workspace denial.
  The sim checks that the workspace is returned wiped and that no
  notification exceeds MTU - 3.

## Behavior

Message for message the reference firmware (`esp32/main/ble_server.c`,
`link_pairing.c`, `zephyr/src/mg_setup.c`), with the contract v2 changes:
`pairing_client_hello` is accepted on a set-up gadget and a confirmed
session's `provision_v2` replaces the stored set in one commit; the SDK
token comes from the platform. A session provisions once, as the Zephyr
reference: after `auth_ok` a further `provision_v2` (or `unpair`,
`wifi_scan`) on it gets the encrypted `error_pairing_confirm_required`, and
a new setup needs a new hello and press; a `provision_v2` that failed
(`error_missing_credentials`, `error_wifi_unsupported`, `error_storage`)
leaves the session usable for a retry (`error_storage` still ends the
connection 500 ms later; a retry that commits first cancels that). Where it
differs, and why:

- The firmware can take the workspace back from a consumed session with
  `mgls_abort()` (for media on the same connection); a record after that
  gets no answer and a disconnect, so a second `provision_v2` is still
  refused.
- A message larger than the workspace's RX buffer gets an encrypted
  `error_storage` on a confirmed session (any other session: reset), then a
  disconnect; the ESP32 drops anything over 8 KB silently.
- Decryption, ECDH and the commit run in `mgls_poll()`, not in the write
  callback; a message that completes while a reply is still going out waits
  for it (writes arriving meanwhile are dropped: the apps never pipeline).
- An unframed write in the middle of a chunked message drops the partial
  message (the reference keeps both).
- Record counters are 32-bit (a larger counter fails as a mismatch would).

## License

The repository's Apache-2.0, except `third_party/micro-ecc` (BSD-2-Clause,
`third_party/micro-ecc/LICENSE.txt`).
