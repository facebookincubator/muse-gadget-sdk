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

// What a log line may say about a secret (the SDK token, a device token, the
// token proof key): whether there is one, its length and the first 4 bytes of
// its SHA-256 as 8 hex digits, never any of its characters (AGENTS.md, Muse
// Gadgets SDK token). tools/redact.py's fingerprint() prints the same.

#include <stdbool.h>

#define TOKEN_FP_TEXT_LEN 40
#define TOKEN_FP_HEX_LEN 9

// The parts, for a screen that lays them out itself (Settings): false for no
// secret; else its length and the first 4 bytes of its SHA-256 as 8 hex
// digits ("" if the hash fails).
bool token_fp_parts(const char *secret, unsigned *len, char hex[TOKEN_FP_HEX_LEN]);

// "none" for NULL or "", else "48 chars, sha256 1a2b3c4d" ("48 chars" alone if
// the hash fails). Returns out.
const char *token_fp_text(const char *secret, char out[TOKEN_FP_TEXT_LEN]);
