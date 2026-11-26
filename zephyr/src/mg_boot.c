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

#include "mg_boot.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/sys/util.h>

static enum mg_boot_mode mode;
static bool by_button;
static bool by_bench;

bool mg_health_safe(void)
{
	return mode == MG_BOOT_SAFE;
}

void mg_health_set_mode(enum mg_boot_mode m)
{
	mode = m;
	by_button = false;
	by_bench = false;
}

void mg_health_set_safe_by_button(void)
{
	mode = MG_BOOT_SAFE;
	by_button = true;
}

bool mg_health_safe_by_button(void)
{
	return mode == MG_BOOT_SAFE && by_button;
}

void mg_health_set_safe_bench(void)
{
	mode = MG_BOOT_SAFE;
	by_button = true; /* as the button's SAFE: open to updates */
	by_bench = true;
}

bool mg_health_safe_bench(void)
{
	return mode == MG_BOOT_SAFE && by_bench;
}

static void seal(struct mg_boot_rec *r);
static void valid_or_zero(struct mg_boot_rec *r);

/* Both seal the record like every other change: an unsealed one reads as
 * power-on at the next boot, which would drop the request (safe next /
 * reset next would restart into a plain normal boot). */
void mg_boot_bench_set(struct mg_boot_rec *r, uint8_t req)
{
	valid_or_zero(r);
	r->flags = (uint8_t)((r->flags & ~MG_BOOT_F_BENCH_MASK) | (req & MG_BOOT_F_BENCH_MASK));
	seal(r);
}

uint8_t mg_boot_bench_take(struct mg_boot_rec *r, bool bench_mode)
{
	valid_or_zero(r);
	uint8_t req = r->flags & MG_BOOT_F_BENCH_MASK;

	r->flags &= (uint8_t)~MG_BOOT_F_BENCH_MASK;
	seal(r);
	return bench_mode ? req : 0;
}

/* CRC-32 (IEEE), bitwise: small, and a few dozen bytes once per boot. */
static uint32_t crc32(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t c = 0xffffffffu;

	while (len--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
	}
	return ~c;
}

static uint32_t rec_crc(const struct mg_boot_rec *r)
{
	return crc32(r, offsetof(struct mg_boot_rec, crc));
}

static void seal(struct mg_boot_rec *r)
{
	r->magic = MG_BOOT_MAGIC;
	r->crc = rec_crc(r);
}

bool mg_boot_valid(const struct mg_boot_rec *r)
{
	return r->magic == MG_BOOT_MAGIC && r->crc == rec_crc(r);
}

static void valid_or_zero(struct mg_boot_rec *r)
{
	if (r->magic != MG_BOOT_MAGIC || r->crc != rec_crc(r)) {
		memset(r, 0, sizeof(*r)); /* power-on: nothing kept */
	}
}

enum mg_boot_mode mg_boot_start(struct mg_boot_rec *r, uint8_t safe_after,
				struct mg_boot_crash *crash, bool *has_crash)
{
	valid_or_zero(r);
	*has_crash = r->has_crash != 0;
	if (*has_crash) {
		*crash = r->crash;
		crash->what[MG_BOOT_WHAT - 1] = '\0';
	}
	r->has_crash = 0;
	memset(&r->crash, 0, sizeof(r->crash));
	uint8_t before = r->unhealthy;

	r->flags &= (uint8_t)~MG_BOOT_F_COUNTED;
	if (r->unhealthy < 255) {
		r->unhealthy++;
		r->flags |= MG_BOOT_F_COUNTED;
	}
	if (r->boots < 0xffff) {
		r->boots++;
	}
	bool safe = safe_after && before >= safe_after;

	if (!safe) {
		r->flags &= (uint8_t)~MG_BOOT_F_SAFE_REACH;
	} else if (r->reach_resets >= safe_after) {
		/* Reach losses alone made this SAFE (one among crashes
		 * doesn't). */
		r->flags |= MG_BOOT_F_SAFE_REACH;
	}
	seal(r);
	return safe ? MG_BOOT_SAFE : MG_BOOT_NORMAL;
}

void mg_boot_healthy(struct mg_boot_rec *r)
{
	valid_or_zero(r);
	r->unhealthy = 0;
	r->flags &= (uint8_t)~MG_BOOT_F_COUNTED;
	seal(r);
}

void mg_boot_peer_restart(struct mg_boot_rec *r)
{
	valid_or_zero(r);
	if ((r->flags & MG_BOOT_F_COUNTED) && r->unhealthy > 0) {
		r->unhealthy--;
	}
	unsigned int n = (r->flags & MG_BOOT_F_PEER_MASK) >> MG_BOOT_F_PEER_SHIFT;

	if (n < 3) {
		n++;
	}
	r->flags = (uint8_t)((r->flags & ~(MG_BOOT_F_COUNTED | MG_BOOT_F_PEER_MASK)) |
			     (n << MG_BOOT_F_PEER_SHIFT));
	r->att_dead = 0;
	seal(r);
}

unsigned int mg_boot_peer_restarts(const struct mg_boot_rec *r)
{
	return (r->flags & MG_BOOT_F_PEER_MASK) >> MG_BOOT_F_PEER_SHIFT;
}

void mg_boot_exchange(struct mg_boot_rec *r)
{
	valid_or_zero(r);
	if (r->att_dead) {
		r->att_dead = 0;
		seal(r);
	}
}

void mg_boot_peer_bound_clear(struct mg_boot_rec *r)
{
	valid_or_zero(r);
	if (r->flags & MG_BOOT_F_PEER_MASK) {
		r->flags &= (uint8_t)~MG_BOOT_F_PEER_MASK;
		seal(r);
	}
}

bool mg_boot_reach_reset(struct mg_boot_rec *r, bool safe, bool lost)
{
	valid_or_zero(r);
	if (safe) {
		if (r->safe_reach >= MG_BOOT_SAFE_REACH_MAX) {
			return false;
		}
		r->safe_reach++;
	}
	/* Never reachable at boot counts as a reach loss too: a SAFE it leads
	 * to goes back by itself after 10 min reachable. */
	if (r->reach_resets < 255) {
		r->reach_resets++;
	}
	if (lost) {
		r->unhealthy = MAX(r->unhealthy, r->reach_resets);
	}
	seal(r);
	return true;
}

bool mg_boot_reach_clear(struct mg_boot_rec *r, bool safe_by_button)
{
	bool back;

	valid_or_zero(r);
	back = (r->flags & MG_BOOT_F_SAFE_REACH) && !safe_by_button;
	r->reach_resets = 0;
	r->safe_reach = 0;
	if (back) {
		r->unhealthy = 0;
		r->flags &= (uint8_t)~(MG_BOOT_F_SAFE_REACH | MG_BOOT_F_COUNTED);
	}
	seal(r);
	return back;
}

void mg_boot_att_dead_set(struct mg_boot_rec *r, uint8_t n)
{
	valid_or_zero(r);
	r->att_dead = n;
	seal(r);
}

void mg_boot_crash(struct mg_boot_rec *r, uint32_t reason, uint32_t pc, uint32_t uptime_s,
		   const char *what)
{
	if (r->magic != MG_BOOT_MAGIC || r->crc != rec_crc(r)) {
		/* Never seal what isn't a record (power-on RAM, or a crash before
		 * mg_boot_start() checked it: the early watchdog's). */
		memset(r, 0, sizeof(*r));
	}
	if (r->has_crash) {
		return; /* the first one tells what went wrong */
	}
	r->has_crash = 1;
	r->crash.reason = reason;
	r->crash.pc = pc;
	r->crash.uptime_s = uptime_s;
	size_t i = 0;

	for (; what && what[i] && i < MG_BOOT_WHAT - 1; i++) {
		r->crash.what[i] = what[i];
	}
	r->crash.what[i] = '\0';
	seal(r);
}

size_t mg_boot_crash_text(const struct mg_boot_crash *c, char *buf, size_t len)
{
	int n;

	if (c->reason == MG_BOOT_REASON_WATCHDOG || c->reason == MG_BOOT_REASON_UNREACHABLE ||
	    c->reason == MG_BOOT_REASON_PEER_LOSS) {
		n = snprintf(buf, len, "%s, up %u s", c->what, (unsigned int)c->uptime_s);
	} else {
		n = snprintf(buf, len, "%s (reason %u) at pc 0x%08x, up %u s", c->what,
			     (unsigned int)c->reason, (unsigned int)c->pc,
			     (unsigned int)c->uptime_s);
	}
	return n < 0 ? 0 : MIN((size_t)n, len ? len - 1 : 0);
}
