/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The security draws' one source: every key,
 * nonce or passkey an attacker must not predict comes from secure_random().
 *
 * esp_fill_random() is a true RNG only while an RF subsystem runs (ESP-IDF's
 * "Random Number Generation": the RF noise is its entropy). Otherwise it is
 * pseudo-random, and a key or nonce drawn then may be predictable. So each
 * draw first checks a radio is on (the BT controller enabled, or Wi-Fi
 * started on a build that links Wi-Fi) and refuses the draw when none is:
 * never a weaker source in its place. The caller refuses the operation
 * (README.md lists each one and how it refuses).
 */

/* True while the BT controller is enabled or Wi-Fi is started; always on the
 * ESP32-P4, whose entropy needs no radio (README.md). No log. */
bool secure_random_radio_on(void);

/* secure_random_radio_on(), and on false the log line
 * "secure_random: no radio on: refusing a security draw", once per spell
 * without a radio (a draw that passes re-arms it). For a security draw that
 * doesn't go through secure_random() itself (PSA key generation). */
bool secure_random_ready(void);

/* n bytes of the hardware RNG into out, and true; or, without a radio,
 * false with out zeroed and nothing drawn. */
bool secure_random(void *out, size_t n);

#ifdef __cplusplus
}
#endif
