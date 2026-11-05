// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "muse_tts_provider.h"
// Session-task only. Worker never accesses chat state. IDs never repeat.
bool muse_tts_busy();
bool muse_tts_start(uint64_t id, const char *text, muse_tts_provider provider);
void muse_tts_cancel();
size_t muse_tts_read(uint64_t id, uint8_t *out, size_t capacity);
bool muse_tts_end(uint64_t id, muse_tts_result *result);
