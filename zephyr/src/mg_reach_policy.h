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

#ifndef MG_REACH_POLICY_H_
#define MG_REACH_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * A boot that isn't reachable is a failed boot, and
 * a gadget must stay reachable. Sampled once a second from plain state (is
 * connectable advertising running, is a link up), never a BLE call:
 *   - a boot not reachable within MG_REACH_BOOT_MS is reset as a failed boot;
 *   - once reachable, MG_REACH_LOST_MS with no advertising and no link (an
 *     update needs a link, so one in progress is never a loss) is reset as a
 *     failed boot, counted across boots (mg_boot_reach_reset());
 *   - a stretch reachable for MG_REACH_CLEAR_MS clears that count;
 *   - exempt time (power off on its way to System OFF; the charge-dark gate
 *     runs before the check starts) counts as reachable and never as a boot.
 * Each boot decides to reset at most once (the caller resets, or stays up
 * inside SAFE when the bound says so).
 *
 * A link is reachability only once an ATT exchange completed on it through
 * the gadget's own path with on-air evidence (a link can form with ATT dead
 * both ways, and the host can answer a peer's MTU request by itself while the
 * next write never completes): one of our own requests answered, or a peer's
 * second request on the link (mg_reach_att_counts()). An exchange the stack
 * answers by itself, a peer's first request and a completed notification
 * never count. An idle link that got there is no loss.
 *
 * Losses a peer could cause (a link up in the unreachable stretch, the
 * ATT-dead links, a stuck TX path) restart the gadget, which sheds the peer,
 * but never count toward SAFE (mg_boot_peer_restart()); only a stretch with
 * no link at all escalates. A link without one counts as unreachable time, and
 * MG_REACH_DEAD_LINKS links in a row that end without one while the gadget's
 * own requests failed (or went unanswered MG_REACH_ATT_WAIT_MS) are a peer
 * loss (mg_reach_link_ended()). A link that ends after an
 * exchange starts the run over; a short one with nothing failed never counts.
 */
#define MG_REACH_BOOT_MS  30000
#define MG_REACH_LOST_MS  30000
#define MG_REACH_CLEAR_MS 600000
#define MG_REACH_DEAD_LINKS 3
#define MG_REACH_ATT_WAIT_MS 5000

enum mg_reach_act {
	MG_REACH_NONE,
	MG_REACH_RESET_BOOT, /* never reachable this boot */
	MG_REACH_RESET_LOST, /* was reachable, then wasn't for MG_REACH_LOST_MS */
	MG_REACH_CLEAR,      /* reachable MG_REACH_CLEAR_MS in a row: the count starts over */
};

struct mg_reach {
	int64_t boot_ms;
	int64_t last_ok_ms;
	int64_t ok_since_ms;
	bool reached;
	bool in_stretch;
	bool cleared;
	bool done;
	bool peer; /* a link was up during the unreachable stretch */
};

static inline void mg_reach_init(struct mg_reach *r, int64_t now)
{
	*r = (struct mg_reach){.boot_ms = now, .last_ok_ms = now};
}

/* A stretch with a link up somewhere in it is one a peer could cause
 * (connectable advertising stops while connected, and a link without a
 * completed exchange isn't reachability): its reset is a peer restart that
 * never counts toward SAFE (mg_boot_peer_restart()). Only a stretch with no
 * link at all (not advertising, nothing connectable) is the gadget's own
 * fault and escalates. */
static inline enum mg_reach_act mg_reach_step_link(struct mg_reach *r, int64_t now,
						   bool reachable, bool exempt, bool link_up)
{
	if (r->done) {
		return MG_REACH_NONE;
	}
	if (reachable || exempt) {
		r->peer = false;
	} else if (link_up) {
		r->peer = true;
	}
	if (exempt) {
		r->last_ok_ms = now;
		if (!r->reached) {
			r->boot_ms = now;
		}
		r->in_stretch = false;
		return MG_REACH_NONE;
	}
	if (reachable) {
		r->reached = true;
		if (!r->in_stretch) {
			r->in_stretch = true;
			r->ok_since_ms = now;
		}
		r->last_ok_ms = now;
		if (!r->cleared && now - r->ok_since_ms >= MG_REACH_CLEAR_MS) {
			r->cleared = true;
			return MG_REACH_CLEAR;
		}
		return MG_REACH_NONE;
	}
	r->in_stretch = false;
	if (!r->reached) {
		if (now - r->boot_ms >= MG_REACH_BOOT_MS) {
			r->done = true;
			return MG_REACH_RESET_BOOT;
		}
		return MG_REACH_NONE;
	}
	if (now - r->last_ok_ms >= MG_REACH_LOST_MS) {
		r->done = true;
		return MG_REACH_RESET_LOST;
	}
	return MG_REACH_NONE;
}

static inline enum mg_reach_act mg_reach_step(struct mg_reach *r, int64_t now, bool reachable,
					      bool exempt)
{
	return mg_reach_step_link(r, now, reachable, exempt, false);
}

/* The restart rate bound: after MG_REACH_PEER_RESTARTS peer restarts since
 * the last token-proven session (or the user's button, or a new image; a
 * plain ATT exchange clears only the ATT-dead run), a peer-caused loss drops
 * the link instead (the gadget keeps advertising, and the owner's update path
 * stays open), except once per boot after MG_REACH_PEER_LATE_MS up. */
#define MG_REACH_PEER_RESTARTS 3
#define MG_REACH_PEER_LATE_MS  600000

/* no_restart: the running image isn't confirmed yet, so a restart would
 * make MCUboot revert it (a peer must never undo an update), or the gadget
 * is in SAFE mode (a restart buys nothing there, and out of the button's
 * SAFE it would put the broken image back): the link goes instead. A peer
 * restart counts as no crash anywhere. */
static inline bool mg_reach_peer_restart_ok(unsigned int restarts, int64_t uptime_ms,
					    bool late_used, bool no_restart)
{
	if (no_restart) {
		return false;
	}
	return restarts < MG_REACH_PEER_RESTARTS ||
	       (uptime_ms >= MG_REACH_PEER_LATE_MS && !late_used);
}

/* After a reset decision: a peer could have caused it. */
static inline bool mg_reach_peer_loss(const struct mg_reach *r)
{
	return r->peer;
}

/* What happened on a link's ATT bearer, and whether it proves the gadget's
 * own path works: only on-air evidence counts. Our own request answered is.
 * A peer's Write Request counts from the second on the link: ATT allows one
 * at a time, so it proves our answer to the one before arrived; the first
 * proves only that the peer's sends get here. A completed notification
 * doesn't count: the host's completion is the controller's Number of
 * Completed Packets, which on both controllers follows the peer's link-layer
 * acknowledgement in normal running, but the Core spec lets a controller
 * report flushed packets the same way (a link coming down), so it is not
 * evidence the peer's ATT got anything. */
enum mg_att_event {
	MG_ATT_STACK_ANSWERED,     /* the host answered by itself (a peer's MTU request) */
	MG_ATT_PEER_FIRST_REQUEST, /* the link's first peer Write Request, answered */
	MG_ATT_PEER_NEXT_REQUEST,  /* a later one: our answer to the one before arrived */
	MG_ATT_NOTIFY_COMPLETED,   /* one of our notifications completed (host-side) */
	MG_ATT_OWN_ANSWERED,       /* one of our requests (MTU, a read) answered */
};

static inline bool mg_reach_att_counts(enum mg_att_event e)
{
	return e == MG_ATT_PEER_NEXT_REQUEST || e == MG_ATT_OWN_ANSWERED;
}

/* Which ATT evidence moves recovery state: on a set-up gadget only a link
 * that passed the gate (this connection's token proof) clears the dead-link
 * run, marks an updated image's link proven or counts as reachable; a
 * stranger's link, its requests refused, moves none of them (nor counts as
 * dead while its ATT works). A gadget that isn't set up has no gate. */
static inline bool mg_reach_exchange_counts(bool att_ok, bool set_up, bool proven)
{
	return att_ok && (!set_up || proven);
}

/* A link counts as reachable when its ATT passed the gate, or while the
 * gadget's own update path is live on it (mg_reach_update_live()): a web
 * updater's unproven, admitted upload is the owner's, and its link is no
 * peer's dead one (a set-up gadget would otherwise restart 30 s into such an
 * upload, "peer: link without ATT 30 s"). It moves nothing else: not the
 * dead-link run, nor an updated image's keep. */
#define MG_REACH_UPDATE_MS 10000

static inline bool mg_reach_link_counts(bool att_gated, bool update_live)
{
	return att_gated || update_live;
}

/* The update path is live on a link: an update chunk the gadget took on it
 * moved within MG_REACH_UPDATE_MS (chunk_recent), or a press admitted an
 * update on it (admitted: its upload is on its way). The update prompt alone
 * (prompting) isn't: any central in range raises one with an unproven update
 * write, and one write every 30 s would keep it up for good, holding the
 * single link while the owner's phone can't connect; a link with nothing
 * else stays a peer's, and the rule above drops it. */
static inline bool mg_reach_update_live(bool chunk_recent, bool prompting, bool admitted)
{
	(void)prompting;
	return chunk_recent || admitted;
}

/* ATT-dead links in a row (one per boot's run). */
struct mg_reach_links {
	uint8_t dead;
};

/* A link ended: att_ok, an ATT exchange completed on it; own_failed, one of
 * the gadget's own requests failed or went unanswered MG_REACH_ATT_WAIT_MS.
 * True on the MG_REACH_DEAD_LINKS-th ATT-dead link in a row: a peer loss. */
static inline bool mg_reach_link_ended(struct mg_reach_links *l, bool att_ok, bool own_failed)
{
	if (att_ok) {
		l->dead = 0;
		return false;
	}
	if (!own_failed) {
		return false; /* a short link with nothing failed: not evidence */
	}
	if (l->dead < MG_REACH_DEAD_LINKS) {
		l->dead++;
	}
	return l->dead >= MG_REACH_DEAD_LINKS;
}

/* The gadget's own request on a link: failed, or still unanswered after the
 * wait when the link ended. */
static inline bool mg_reach_own_failed(bool failed, bool pending, int64_t asked_ms, int64_t end_ms)
{
	return failed || (pending && end_ms - asked_ms >= MG_REACH_ATT_WAIT_MS);
}

#endif /* MG_REACH_POLICY_H_ */
