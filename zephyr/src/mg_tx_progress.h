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

#ifndef MG_TX_PROGRESS_H_
#define MG_TX_PROGRESS_H_

#include <stdbool.h>
#include <stdint.h>

/* The BLE TX path is progress too. A live host whose TX path is dead must
 * reset: with a link up, something owed to the link and nothing completed
 * for MG_TX_DEAD_MS counts as no progress.
 *
 * Two host-side signals, each its own struct mg_txp, the same on Zephyr's
 * open controller and the SoftDevice Controller (both use the Zephyr host's
 * HCI flow control):
 *  - notifications: ours in flight or waiting for an ATT buffer, against
 *    their sent callbacks (mg_ble.c);
 *  - the ACL packets: those the controller holds (the connection's credit
 *    semaphore below its limit), against the completions it reports (the
 *    host's Number of Completed Packets, any sender: mg_txp_acl()). This one
 *    sees an ATT response stuck behind held packets, to any write, with no
 *    notification after it (a first write that is a CCC subscription, say).
 * Completions are counted, not thread or work-queue liveness, so a queue kept
 * slow (repeated I2C timeouts on the system work queue, where the SDC boards
 * run the sent callbacks) can't hide a dead path, and a slow-but-moving one
 * is never a loss.
 *
 * The clock: 10 s when the peer is evidently alive (a write or any other
 * packet from it arrived since the stall began); otherwise the link's own
 * supervision timeout plus a margin, at most 30 s, so a phone that walks away
 * mid-send ends in the controller's clean disconnect, never a restart. This
 * is the detection threshold: the supervisor then restarts the gadget as a
 * peer loss within a second (mg_health_peer_loss()). */
#ifndef MG_TX_DEAD_MS /* a test may shorten the clock */
#define MG_TX_DEAD_MS          10000
#endif
#define MG_TX_SILENT_MARGIN_MS 2000
#define MG_TX_SILENT_MAX_MS    30000 /* a held dead link ends within 30 s */

struct mg_txp {
	bool owed;      /* something is waiting on the link */
	uint32_t since; /* ms: the last completion, or when owing started */
};

static inline void mg_txp_init(struct mg_txp *p)
{
	p->owed = false;
	p->since = 0;
}

/* Something is now waiting (sent, or waiting for a buffer or a credit). */
static inline void mg_txp_owe(struct mg_txp *p, uint32_t now)
{
	if (!p->owed) {
		p->owed = true;
		p->since = now;
	}
}

/* A completion: the path moves. still_owed: more is waiting after it. */
static inline void mg_txp_done(struct mg_txp *p, uint32_t now, bool still_owed)
{
	p->since = now;
	p->owed = still_owed;
}

/* Nothing waits (all completed, or the link is gone). */
static inline void mg_txp_idle(struct mg_txp *p)
{
	p->owed = false;
}

/* The stall's limit: last_rx (ms) is the peer's latest packet we saw,
 * sup_timeout_ms the link's supervision timeout. */
static inline uint32_t mg_txp_limit(const struct mg_txp *p, uint32_t last_rx,
				    uint32_t sup_timeout_ms)
{
	uint32_t silent = sup_timeout_ms + MG_TX_SILENT_MARGIN_MS;

	if ((int32_t)(last_rx - p->since) >= 0) {
		return MG_TX_DEAD_MS; /* the peer spoke during the stall */
	}
	return silent < MG_TX_SILENT_MAX_MS ? silent : MG_TX_SILENT_MAX_MS;
}

static inline bool mg_txp_dead(const struct mg_txp *p, uint32_t now, bool link_up,
			       uint32_t last_rx, uint32_t sup_timeout_ms)
{
	return link_up && p->owed && now - p->since >= mg_txp_limit(p, last_rx, sup_timeout_ms);
}

/* The ACL leg, sampled every 100 ms (mg_ble.c): held = packets the
 * controller holds now, done = completions it has reported (a counter that
 * only grows). A completion since the last sample is progress whatever the
 * occupancy; none held is idle; held with none completed owes. */
static inline void mg_txp_acl(struct mg_txp *p, uint32_t now, unsigned int held,
			      unsigned int done, unsigned int *last_done)
{
	if (done != *last_done) {
		mg_txp_done(p, now, held != 0);
	} else if (held == 0) {
		mg_txp_idle(p);
	} else {
		mg_txp_owe(p, now);
	}
	*last_done = done;
}

#endif /* MG_TX_PROGRESS_H_ */
