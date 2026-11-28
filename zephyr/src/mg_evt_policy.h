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
 * A central's traffic never holds the app work queue: the watchdog's ping
 * and the healthy mark run on that queue, and a stuck app work queue counts
 * as a crash. So:
 * - the event handler takes at most MG_EVT_RUN_MAX events, or MG_EVT_RUN_MS
 *   of time, per run, then queues itself again behind whatever else waits;
 * - the replies one event sends wait for notification buffers at most
 *   MG_REPLY_BUDGET_MS together (each at most 500 ms, as before);
 * - request_status is answered at most MG_STATUS_PER_WINDOW times per
 *   MG_STATUS_WINDOW_MS on a connection (it needs no proof to ask);
 * - an app work queue stale while peer events have been backed up for half
 *   the watchdog's staleness with a link up is a peer loss (a soft restart
 *   that never counts toward SAFE), not a crash.
 */
#ifndef MG_EVT_POLICY_H
#define MG_EVT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define MG_EVT_RUN_MAX       4
#define MG_EVT_RUN_MS        50
#define MG_REPLY_BUDGET_MS   1000
#define MG_REPLY_EACH_MS     500
#define MG_STATUS_PER_WINDOW 4
#define MG_STATUS_WINDOW_MS  1000

/* Another event this run? (done: events handled so far this run.) */
static inline bool mg_evt_run_more(unsigned int done, int64_t elapsed_ms)
{
	return done < MG_EVT_RUN_MAX && elapsed_ms < MG_EVT_RUN_MS;
}

/* How long a reply may wait for a buffer: the event's budget left, at most
 * MG_REPLY_EACH_MS; deadline 0 means no event's budget runs (500 ms). */
static inline int32_t mg_reply_wait_ms(int64_t now, int64_t deadline)
{
	if (deadline == 0) {
		return MG_REPLY_EACH_MS;
	}
	int64_t left = deadline - now;

	return left <= 0 ? 0 : left > MG_REPLY_EACH_MS ? MG_REPLY_EACH_MS : (int32_t)left;
}

struct mg_rate {
	int64_t window_start;
	unsigned int n;
};

/* One more in the window, or false (and nothing counted) when it's full. */
static inline bool mg_rate_take(struct mg_rate *r, int64_t now)
{
	if (r->n == 0 || now - r->window_start >= MG_STATUS_WINDOW_MS) {
		r->window_start = now;
		r->n = 0;
	}
	if (r->n >= MG_STATUS_PER_WINDOW) {
		return false;
	}
	r->n++;
	return true;
}

/* Peer events backed up (since busy_since, 0: none) long enough, with a link
 * up, AND the event handler still taking them (last_took: when it last took
 * one, 0: never) within that half window: only then is a stale app work
 * queue the peer's doing. A queue stuck in anything else (a handler, a flash
 * wait) never runs the handler, so its events wait untaken and it stays a
 * crash, whatever the peer sends meanwhile. */
static inline bool mg_evt_saturated(bool link_up, int64_t busy_since, int64_t last_took,
				    int64_t now, uint32_t stale_ms)
{
	int64_t half = (int64_t)(stale_ms / 2);

	return link_up && busy_since != 0 && now - busy_since >= half && last_took != 0 &&
	       now - last_took < half;
}

#endif /* MG_EVT_POLICY_H */
