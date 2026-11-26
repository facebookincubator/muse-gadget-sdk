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

#ifndef MGLS_UTIL_H_
#define MGLS_UTIL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mg_link_setup.h"

/*
 * Internal helpers of the setup core: a strict, non-recursive JSON reader
 * for flat objects (RFC 8259; nested members are validated and reported as
 * MGJ_NESTED), a JSON writer, base64url and decimal parsing. No allocation.
 */

enum mgj_type {
	MGJ_STRING,
	MGJ_NUMBER,
	MGJ_TRUE,
	MGJ_FALSE,
	MGJ_NULL,
	MGJ_NESTED,
};

#define MGJ_MAX_DEPTH 32

/*
 * Parses one JSON object in buf[0..len) (len < 65536) in place: strings
 * (keys too) are unescaped and NUL-terminated inside buf; fields get their
 * offsets. Refuses anything but one object with optional whitespace around
 * it, duplicate keys, raw control characters, \u0000, lone surrogates, more
 * than maxf members and nesting deeper than MGJ_MAX_DEPTH. 0 on success.
 */
int mgj_parse(char *buf, size_t len, mgls_jfield_t *f, size_t maxf, uint8_t *nout);
const mgls_jfield_t *mgj_get(const char *base, const mgls_jfield_t *f, size_t n, const char *key);
/* The member's string value (NUL-terminated), or NULL if absent / not a
 * string. */
const char *mgj_str(const char *base, const mgls_jfield_t *f, size_t n, const char *key);
/* An integer member (no fraction or exponent, within int32). */
bool mgj_int(const char *base, const mgls_jfield_t *fld, int32_t *out);

/* Writer: {"k":"v","n":1}, strings escaped; all bounded. */
typedef struct mgj_w {
	char *buf;
	size_t cap;
	size_t len;
	bool err;
} mgj_w;

void mgj_begin(mgj_w *w, char *buf, size_t cap);
void mgj_raw(mgj_w *w, const char *s, size_t n);
void mgj_cstr(mgj_w *w, const char *s); /* raw, NUL-terminated */
void mgj_u32(mgj_w *w, uint32_t v);     /* raw decimal */
void mgj_kstr(mgj_w *w, const char *key, const char *val);
void mgj_kstrn(mgj_w *w, const char *key, const char *val, size_t n);
void mgj_kint(mgj_w *w, const char *key, uint32_t val);
void mgj_kraw(mgj_w *w, const char *key, const char *val); /* unquoted */
/* Closes the object; NUL-terminates; the length, or -1 if it overflowed. */
int mgj_end(mgj_w *w);
/* NUL-terminates without closing an object (the transcript); -1 on
 * overflow. */
int mgj_finish(mgj_w *w);

/* base64url without padding. encode: the length (NUL added) or -1. decode:
 * refuses padding, other characters, an empty input and an impossible
 * length; safe in place (out == in). 0 on success. */
int mgls_b64_encode(const uint8_t *in, size_t len, char *out, size_t cap);
int mgls_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap, size_t *out_len);

/* Plain decimal, no sign, overflow checked (a record counter above 2^32 - 1
 * can never be the expected one, so it fails like any other mismatch). */
bool mgls_parse_u32(const char *s, uint32_t *out);

#endif
