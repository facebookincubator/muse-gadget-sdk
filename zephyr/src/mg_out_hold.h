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

#ifndef MG_OUT_HOLD_H
#define MG_OUT_HOLD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Mic output goes only to a
 * central that passed this connection's token proof. A capture before the
 * proof is held whole, in order (its change_data_type, Data, the gestures
 * and stop_mic behind it), and goes once the proof matches; a failed proof
 * or the link's end discards it; held output not released within
 * MG_OUT_STALE_MS of the capture's end is discarded. This is the hold: a
 * FIFO of records [kind][len (2, LE)][bytes] in one buffer the caller lends.
 * Portable; host-tested (tests/host/test_gadget.c).
 */

#define MG_OUT_STALE_MS 3000 /* the output hold's staleness bound */

enum mg_out_kind {
	MG_OUT_CTRL = 1, /* a Control message (change_data_type, gesture, stop_mic) */
	MG_OUT_DATA = 2, /* a Data notification (audio frames) */
};

struct mg_out_hold {
	uint8_t *buf;
	size_t cap;
	size_t head; /* the oldest record */
	size_t len;  /* bytes held, records' headers included */
	uint32_t records;
	uint32_t data_bytes; /* Data payload bytes held */
};

static inline void mg_out_hold_init(struct mg_out_hold *h, uint8_t *buf, size_t cap)
{
	h->buf = buf;
	h->cap = cap;
	h->head = h->len = 0;
	h->records = h->data_bytes = 0;
}

static inline void mg_out_hold_clear(struct mg_out_hold *h)
{
	h->head = h->len = 0;
	h->records = h->data_bytes = 0;
}

/* Appends one record; false (nothing kept) if it doesn't fit whole. */
static inline bool mg_out_hold_push(struct mg_out_hold *h, uint8_t kind, const uint8_t *p,
				    size_t n)
{
	if (h->buf == NULL || n > 0xffff || h->len + 3 + n > h->cap) {
		return false;
	}
	/* Records are kept contiguous: the buffer is linear, emptied only by
	 * popping from the front (or cleared), so the tail is head + len. */
	if (h->head + h->len + 3 + n > h->cap) {
		memmove(h->buf, &h->buf[h->head], h->len);
		h->head = 0;
	}
	uint8_t *d = &h->buf[h->head + h->len];

	d[0] = kind;
	d[1] = (uint8_t)n;
	d[2] = (uint8_t)(n >> 8);
	memcpy(&d[3], p, n);
	h->len += 3 + n;
	h->records++;
	if (kind == MG_OUT_DATA) {
		h->data_bytes += (uint32_t)n;
	}
	return true;
}

/* The oldest record (pointer into the buffer), without removing it; false
 * when empty. */
static inline bool mg_out_hold_peek(const struct mg_out_hold *h, uint8_t *kind,
				    const uint8_t **p, size_t *n)
{
	if (h->len == 0) {
		return false;
	}
	const uint8_t *s = &h->buf[h->head];

	*kind = s[0];
	*n = (size_t)s[1] | (size_t)s[2] << 8;
	*p = &s[3];
	return true;
}

/* Removes the oldest record (after it was sent or saved). */
static inline void mg_out_hold_pop(struct mg_out_hold *h)
{
	uint8_t kind;
	const uint8_t *p;
	size_t n;

	if (!mg_out_hold_peek(h, &kind, &p, &n)) {
		return;
	}
	h->head += 3 + n;
	h->len -= 3 + n;
	h->records--;
	if (kind == MG_OUT_DATA) {
		h->data_bytes -= (uint32_t)n;
	}
	if (h->len == 0) {
		h->head = 0;
	}
}

/* Whether held output is past its time: the capture ended at end_ms (0:
 * still capturing) and MG_OUT_STALE_MS went by since. */
static inline bool mg_out_hold_stale(uint32_t now_ms, uint32_t end_ms)
{
	return end_ms != 0 && (uint32_t)(now_ms - end_ms) >= MG_OUT_STALE_MS;
}

#endif /* MG_OUT_HOLD_H */
