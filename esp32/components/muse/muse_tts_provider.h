// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>

// Providers must declare format before writing. Audio is a continuous elementary
// stream: transport framing (JSON, base64, WebSocket) belongs in the adapter.
enum class muse_tts_format { mp3, unsupported };
enum class muse_tts_result { ok, failed, cancelled };
struct muse_tts_sink {
    void *context;
    bool (*format)(void *, muse_tts_format);
    // Blocks with backpressure; false means stop. Never discard a partial write.
    bool (*write)(void *, const uint8_t *, size_t);
    bool (*cancelled)(void *);
};
using muse_tts_provider = muse_tts_result (*)(const char *, const muse_tts_sink &);
// Called on the session task before synthesis. Return nullptr when unavailable
// (for example, missing configuration). Override the weak default in your
// application/component; own provider credentials and build settings there.
// The returned function runs synchronously on the TTS worker. Do not retain the
// request text or sink beyond that call, or access session state from it.
muse_tts_provider muse_tts_get_provider();
