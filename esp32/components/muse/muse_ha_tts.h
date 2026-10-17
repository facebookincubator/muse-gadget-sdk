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

/*
 * Speaks Muse's replies with Home Assistant's TTS (CONFIG_HA_TTS): asks HA
 * for each reply as MP3 (/api/tts_get_url) and streams it back, and the
 * session reads it with muse_ha_tts_read() and decodes it as if Muse had sent
 * it. A task of its own does the HTTP, so the session's task never blocks on it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MUSE_HA_TTS_IDLE,
    MUSE_HA_TTS_RUNNING,
    MUSE_HA_TTS_OK,
    MUSE_HA_TTS_FAILED,
} muse_ha_tts_state_t;

/* Starts the task. Safe to call more than once. */
void muse_ha_tts_start(void);

/* Starts fetching `text` as MP3. False while a fetch is still running, or if
 * it can't queue: the caller shows the reply unspoken rather than wait. */
bool muse_ha_tts_fetch(const char *text);

/* Copies up to `cap` bytes of fetched MP3 without blocking. */
size_t muse_ha_tts_read(void *buf, size_t cap);

/* The fetch's state. Read it before draining with muse_ha_tts_read():
 * once it's OK or FAILED, whatever is left to read is all there is. */
muse_ha_tts_state_t muse_ha_tts_state(void);

/* MP3 bytes the current fetch has handed over so far. */
size_t muse_ha_tts_bytes(void);

/* Abandons the current fetch, without blocking: once its request is open,
 * the socket is shut down, which fails a blocked read; before that, the fetch
 * sees the cancel when open returns. The 15 s timeout bounds TCP connect and
 * individual TLS receives, not the whole blocking handshake or DNS lookup.
 * The session watchdog falls back independently. Then the state goes to FAILED. */
void muse_ha_tts_cancel(void);
