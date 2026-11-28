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

#include "mgls_store.h"

#include <string.h>

#include "mgls_crypto.h"

#define VERSION     1
#define MAGIC       "MGS2"
#define NO_TOKEN    1 /* MGTP_NO_TOKEN */
#define UNREADABLE  (-2) /* mglss_t.cur: a slot couldn't be read, nothing known (mglss_set_up() true) */
#define CHUNK       MGLSS_UNIT

typedef char mglss_check_unit[(MGLSS_UNIT >= 8 && MGLSS_UNIT % 4 == 0) ? 1 : -1];
typedef char mglss_check_record[(MGLSS_SLOT_NEED <= MGLSS_RECORD_MAX) ? 1 : -1];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

static uint32_t get32(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
	       ((uint32_t)b[3] << 24);
}

static void put32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

static uint16_t get16(const uint8_t *b)
{
	return (uint16_t)(b[0] | (b[1] << 8));
}

static uint32_t record_len(uint32_t al, uint32_t rl)
{
	return MGLSS_HDR_LEN + al + rl + 4;
}

static uint32_t commit_off(uint32_t al, uint32_t rl)
{
	return (record_len(al, rl) + CHUNK - 1) / CHUNK * CHUNK;
}

/* Streams the record out in aligned CHUNK units through a small buffer. */
struct writer {
	const mglss_flash_t *f;
	uint32_t addr;
	uint8_t buf[CHUNK];
	uint32_t fill;
	uint32_t crc;
	int err;
};

static void w_put(struct writer *w, const void *data, uint32_t n)
{
	const uint8_t *p = data;

	while (n > 0 && !w->err) {
		uint32_t take = CHUNK - w->fill;

		if (take > n) {
			take = n;
		}
		memcpy(&w->buf[w->fill], p, take);
		w->fill += take;
		p += take;
		n -= take;
		if (w->fill == CHUNK) {
			w->err = w->f->write(w->f->ctx, w->addr, w->buf, CHUNK);
			w->addr += CHUNK;
			w->fill = 0;
		}
	}
}

static void w_rec(struct writer *w, const void *data, uint32_t n)
{
	w->crc = crc32_update(w->crc, data, n);
	w_put(w, data, n);
}

static void w_flush(struct writer *w)
{
	if (w->fill > 0 && !w->err) {
		memset(&w->buf[w->fill], 0xFF, CHUNK - w->fill);
		w->err = w->f->write(w->f->ctx, w->addr, w->buf, CHUNK);
		w->addr += CHUNK;
		w->fill = 0;
	}
}

/* Validates slot i: 0 and its header, -1 (not a valid record), or -2 (a read failed: nothing known). */
static int check_slot(const mglss_flash_t *f, int i, uint8_t hdr[MGLSS_HDR_LEN])
{
	uint32_t base = f->slot_addr[i];
	uint8_t buf[CHUNK];
	uint32_t al, rl, len, crc, off;

	if (f->read(f->ctx, base, hdr, MGLSS_HDR_LEN)) {
		return -2;
	}
	al = get16(&hdr[6]);
	rl = get16(&hdr[8]);
	if (hdr[5] != VERSION || al == 0 || rl == 0 || al > MGLS_ACCESS_TOKEN_MAX ||
	    rl > MGLS_REFRESH_TOKEN_MAX || commit_off(al, rl) + CHUNK > f->slot_size) {
		return -1;
	}
	if (f->read(f->ctx, base + commit_off(al, rl), buf, 4)) {
		return -2;
	}
	if (memcmp(buf, MAGIC, 4) != 0) {
		return -1;
	}
	len = record_len(al, rl) - 4;
	crc = crc32_update(0, hdr, MGLSS_HDR_LEN);
	for (off = MGLSS_HDR_LEN; off < len;) {
		uint32_t n = len - off < CHUNK ? len - off : CHUNK;

		if (f->read(f->ctx, base + off, buf, n)) {
			return -2;
		}
		crc = crc32_update(crc, buf, n);
		off += n;
	}
	if (f->read(f->ctx, base + len, buf, 4)) {
		return -2;
	}
	return get32(buf) == crc ? 0 : -1;
}

/* Whether a slot holds anything but erased flash (a torn erase can leave
 * old token bytes behind an erased header, so the whole slot is read). */
static bool slot_dirty(const mglss_flash_t *f, int i)
{
	uint8_t b[CHUNK];

	for (uint32_t off = 0; off < f->slot_size; off += CHUNK) {
		uint32_t n = f->slot_size - off < CHUNK ? f->slot_size - off : CHUNK;

		if (f->read(f->ctx, f->slot_addr[i] + off, b, n)) {
			return true;
		}
		for (uint32_t k = 0; k < n; k++) {
			if (b[k] != 0xFF) {
				mglsc_wipe(b, sizeof(b));
				return true;
			}
		}
	}
	return false;
}

int mglss_load(mglss_t *st, const mglss_flash_t *f)
{
	uint8_t h0[MGLSS_HDR_LEN], h1[MGLSS_HDR_LEN];
	int c0, c1, keep, err = 0;
	bool v0, v1;

	memset(st, 0, sizeof(*st));
	st->f = f;
	st->cur = -1;
	c0 = check_slot(f, 0, h0);
	c1 = check_slot(f, 1, h1);
	if (c0 == -2 || c1 == -2) {
		/* A slot couldn't be read: erase nothing and choose nothing (the next boot reads it again). Unreadable
		 * isn't empty: the state says so (cur -2) until a load reads both slots, and mglss_set_up() is true. */
		mglsc_wipe(h0, sizeof(h0));
		mglsc_wipe(h1, sizeof(h1));
		st->cur = UNREADABLE;
		return -2;
	}
	v0 = c0 == 0;
	v1 = c1 == 0;
	if (v0 && v1) {
		keep = (int32_t)(get32(h1) - get32(h0)) > 0 ? 1 : 0;
	} else {
		keep = v0 ? 0 : (v1 ? 1 : -1);
	}
	for (int i = 0; i < 2; i++) {
		/* A torn commit, or an old set a power loss kept: erase it. */
		if (i != keep && slot_dirty(f, i) &&
		    f->erase(f->ctx, f->slot_addr[i], f->slot_size) != 0) {
			err = -1;
		}
	}
	if (keep >= 0) {
		const uint8_t *h = keep ? h1 : h0;

		st->cur = (int8_t)keep;
		st->seq = get32(h);
		st->flags = h[4];
		st->access_len = get16(&h[6]);
		st->refresh_len = get16(&h[8]);
	}
	mglsc_wipe(h0, sizeof(h0));
	mglsc_wipe(h1, sizeof(h1));
	return err;
}

bool mglss_set_up(const mglss_t *st)
{
	/* A slot that couldn't be read may hold a set: fail closed (a re-setup, never a first setup). */
	return st->cur == UNREADABLE || (st->cur >= 0 && (st->flags & MGLS_CRED_SETUP_COMPLETE));
}

/* A set is loaded and readable (K and the tokens can be read). */
static bool loaded(const mglss_t *st)
{
	return st->cur >= 0 && (st->flags & MGLS_CRED_SETUP_COMPLETE);
}

int mglss_commit(mglss_t *st, const mgls_credentials_t *c)
{
	const mglss_flash_t *f = st->f;
	int target, old;
	uint32_t base, seq;
	uint8_t hdr[12], unit[CHUNK], chk[MGLSS_HDR_LEN];
	struct writer w;

	if (c->access_len == 0 || c->refresh_len == 0 || c->access_len > MGLS_ACCESS_TOKEN_MAX ||
	    c->refresh_len > MGLS_REFRESH_TOKEN_MAX ||
	    commit_off((uint32_t)c->access_len, (uint32_t)c->refresh_len) + CHUNK > f->slot_size) {
		return -1;
	}
	/* No set loaded: both slots empty, or a load that couldn't read one (it may hold a set whose seq is above any
	 * chosen here, which would win at the next boot). Load again; a slot still unreadable: commit nothing. */
	if (st->cur < 0 && mglss_load(st, f) == -2) {
		return -2;
	}
	old = st->cur;
	target = old == 0 ? 1 : 0;
	base = f->slot_addr[target];
	seq = old >= 0 ? st->seq + 1 : 1;
	if (f->erase(f->ctx, base, f->slot_size)) {
		return -1;
	}
	put32(&hdr[0], seq);
	hdr[4] = c->flags;
	hdr[5] = VERSION;
	hdr[6] = (uint8_t)c->access_len;
	hdr[7] = (uint8_t)(c->access_len >> 8);
	hdr[8] = (uint8_t)c->refresh_len;
	hdr[9] = (uint8_t)(c->refresh_len >> 8);
	hdr[10] = 0;
	hdr[11] = 0;

	memset(&w, 0, sizeof(w));
	w.f = f;
	w.addr = base;
	w_rec(&w, hdr, sizeof(hdr));
	w_rec(&w, c->k, 32);
	w_rec(&w, c->access_token, (uint32_t)c->access_len);
	w_rec(&w, c->refresh_token, (uint32_t)c->refresh_len);
	put32(unit, w.crc);
	w_put(&w, unit, 4);
	w_flush(&w);
	mglsc_wipe(w.buf, sizeof(w.buf));
	/* The commit unit, last and on its own. */
	if (!w.err) {
		memset(unit, 0xFF, sizeof(unit));
		memcpy(unit, MAGIC, 4);
		w.err = f->write(f->ctx,
				 base + commit_off((uint32_t)c->access_len, (uint32_t)c->refresh_len),
				 unit, CHUNK);
	}
	/* Read it all back. */
	if (w.err || check_slot(f, target, chk) != 0 || get32(chk) != seq) {
		(void)f->erase(f->ctx, base, f->slot_size);
		mglsc_wipe(chk, sizeof(chk));
		return -1;
	}
	mglsc_wipe(chk, sizeof(chk));
	/* The old set goes now: never two token sets. A power loss here leaves it to mglss_load() at the next boot
	 * (lower seq). With no set loaded the other slot holds no valid set (the load above checked), but a torn
	 * erase can leave token bytes there: they go too. */
	if (old >= 0 || slot_dirty(f, 1 - target)) {
		int o = 1 - target;

		if (f->erase(f->ctx, f->slot_addr[o], f->slot_size) != 0) {
			if (old >= 0 && check_slot(f, o, chk) == 0) {
				/* The old set still stands: take the new one back, so the commit fails with the old set as
				 * it was. If that erase fails too, the state is what the slots hold (the new set, newer). */
				mglsc_wipe(chk, sizeof(chk));
				if (f->erase(f->ctx, base, f->slot_size) != 0 && mglss_load(st, f) == -2) {
					return -2; /* the state kept unreadable (mglss_set_up() true) */
				}
				return -1;
			}
			/* No valid set left there (the erase tore it, or none was): the new set is the only one;
			 * mglss_load() erases what remains at the next boot. Not silent: the board logs it. */
			mglsc_wipe(chk, sizeof(chk));
			if (f->log) {
				f->log(f->ctx, "mglss: the old slot's erase failed; its leftover bytes go at the next boot");
			}
		}
	}
	st->cur = (int8_t)target;
	st->seq = seq;
	st->flags = c->flags;
	st->access_len = (uint16_t)c->access_len;
	st->refresh_len = (uint16_t)c->refresh_len;
	return 0;
}

int mglss_erase(mglss_t *st)
{
	const mglss_flash_t *f = st->f;
	int err = 0;
	int first = st->cur >= 0 ? st->cur : 0;

	/* The valid slot first: once it is gone the set is gone. */
	for (int k = 0; k < 2; k++) {
		int i = k == 0 ? first : 1 - first;

		if (f->erase(f->ctx, f->slot_addr[i], f->slot_size) != 0) {
			err = -1;
		}
	}
	if (err) {
		/* What the slots hold now: a set whose slot didn't erase stays set up, as the next boot would find it
		 * (the caller reports the failure). */
		if (mglss_load(st, f) == -2) {
			return -2; /* the state kept unreadable (mglss_set_up() true) */
		}
		return err;
	}
	st->cur = -1;
	st->seq = 0;
	st->flags = 0;
	st->access_len = 0;
	st->refresh_len = 0;
	return err;
}

int mglss_get_k(const mglss_t *st, uint8_t k[32])
{
	if (st->cur == UNREADABLE) {
		return -1; /* not known: an error, never "no token" */
	}
	if (!loaded(st)) {
		return NO_TOKEN;
	}
	if (st->f->read(st->f->ctx, st->f->slot_addr[st->cur] + 12, k, 32)) {
		mglsc_wipe(k, 32);
		return -1;
	}
	return 0;
}

int mglss_read_token(const mglss_t *st, bool refresh, char *out, size_t cap, size_t *len)
{
	uint32_t off = MGLSS_HDR_LEN + (refresh ? st->access_len : 0u);
	size_t n = refresh ? st->refresh_len : st->access_len;

	if (!loaded(st) || n > cap) {
		return -1;
	}
	if (st->f->read(st->f->ctx, st->f->slot_addr[st->cur] + off, out, (uint32_t)n)) {
		return -1;
	}
	*len = n;
	return 0;
}
