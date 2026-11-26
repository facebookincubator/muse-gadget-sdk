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

# secure_random

The security draws' one source.
On an ESP32, `esp_fill_random()` is a true RNG only while an RF subsystem (BT
or Wi-Fi) runs; otherwise it is pseudo-random. So every draw an attacker must
not predict goes through `secure_random(out, n)`: it checks the BT controller
is enabled (`esp_bt_controller_get_status()`) or Wi-Fi is started (on builds
that link Wi-Fi: `esp_wifi_get_max_tx_power()` answers only after
`esp_wifi_start()`), and otherwise returns false with the buffer zeroed and
nothing drawn, logging `E secure_random: no radio on: refusing a security
draw` once per spell without a radio. The ESP32-P4, with no radio of its own
(its Wi-Fi is a coprocessor's, through ESP-Hosted), needs none: ESP-IDF keeps
its SAR ADC entropy source on from the application's start, so its draws
always pass. The caller then refuses the operation;
nothing falls back to a weaker source. `secure_random_ready()` is the same
check for a draw that doesn't come through here (PSA key generation).

A tiny component of its own, needing only ESP-IDF, so `main/`,
`muse_gadget_ble`, `muse` and `noise_core` all take it without a dependency
cycle. BLE-only images (`CONFIG_MUSE_GADGET_BLE_STANDALONE`) never name a
Wi-Fi symbol, so the Wi-Fi stack stays out of them.

The security draws, each refusing on its operation's existing error path:

| Draw | Refusal |
|---|---|
| Link setup's P-256 ephemeral key and ECDH blinding (`main/link_pairing.c`, `rng_cb()`, mbedTLS `f_rng`) | `MBEDTLS_ERR_ECP_RANDOM_FAILED`; the hello answers `error_pairing_unavailable` |
| Link setup's device nonce (`link_pairing.c`) | `error_pairing_unavailable` |
| The token proof's device nonce (`muse_gadget_ble/mg_ble.c`, `op_random()`) | the proof's existing failure (`busy`); the seal takes its nonces from the proof and draws nothing |
| Muse phone setup's BLE passkey (`muse/muse_ble.c`) | the pairing is refused (the link is closed) |
| The passcode lock's 16-byte salt (`muse/muse_passcode_esp.c`) | the passcode isn't set (Settings > Passcode stays off; logged on `muse_pass`) |
| Noise's ephemeral X25519 keys and random bytes (`noise_core`, `PsaCryptoBackend`, `NOISE_CORE_SECURE_RANDOM`) | `FailedPrecondition` |

Not security draws, so they keep `esp_random()` / `esp_fill_random()`
(`tests/test_secure_random.py` holds the list and fails on any new call):
request and call ids (`muse_chat_link.c`, `muse_chat_session.cpp`,
`noise_control.cpp`'s `make_uuid()`, `muse_voice_call.cpp`'s call id: names,
sent inside the encrypted session, never secrets), WebSocket masking keys
(`noise_control.cpp`, `muse_chat_session.cpp`: RFC 6455's masking is against
proxy cache poisoning on plain HTTP; these frames travel inside TLS or Noise),
the power test's boot seed (`muse_power_test.c`) and the data-path bench's
test data (`pipeline_bench.cpp`).
