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

#include "mg_linkpol.h"

#include <stdio.h>
#include <string.h>

void mg_linkpol_init(mg_linkpol_t *p)
{
    uint16_t itvl = p->itvl ? p->itvl : MG_LINKPOL_ITVL;
    uint16_t lat = p->ready_latency;
    bool tuned = p->itvl != 0;
    bool wake_off = p->wake_off;   /* the bench switch survives a reconnect too */
    bool sleep_tuned = p->sleep_tuned;   /* ...and the sleep set's knob, and its last grant */
    mg_linkpol_params_t sleep_set = p->sleep_set;
    bool grant = p->sleep_grant_valid;
    uint16_t g_itvl = p->sleep_grant_itvl, g_lat = p->sleep_grant_latency, g_to = p->sleep_grant_timeout;
    int64_t g_us = p->sleep_grant_us;
    memset(p, 0, sizeof(*p));
    p->wake_off = wake_off;
    p->sleep_tuned = sleep_tuned;
    p->sleep_set = sleep_set;
    p->sleep_grant_valid = grant;
    p->sleep_grant_itvl = g_itvl;
    p->sleep_grant_latency = g_lat;
    p->sleep_grant_timeout = g_to;
    p->sleep_grant_us = g_us;
    p->itvl = itvl;
    p->ready_latency = tuned ? lat : MG_LINKPOL_LATENCY_READY;
    p->mode = MG_LINKPOL_READY;
    p->app = MG_LINKPOL_READY;
    p->reported = MG_LINKPOL_NO_MODE;
}

void mg_linkpol_connect(mg_linkpol_t *p, int64_t now_us, uint16_t itvl, uint16_t latency, uint16_t timeout)
{
    /* A policy nothing initialized yet (mg_ble's, static zero until its first
     * connection) takes init's defaults: a 0 interval would ask "0-0", which
     * the stack refuses. The bench knobs survive a reconnect. */
    mg_linkpol_init(p);
    p->on = true;
    p->connect_us = now_us;
    p->cur_itvl = itvl;
    p->cur_latency = latency;
    p->cur_timeout = timeout;
    p->ready_since_us = now_us;
    p->owed = true;   /* ready, after the central's first update or 1 s */
    p->mode_changed = true;
    p->why = "connect";
}

const char *mg_linkpol_mode_name(uint8_t mode)
{
    switch (mode) {
    case MG_LINKPOL_SLEEP: return "sleep";
    case MG_LINKPOL_READY: return "ready";
    case MG_LINKPOL_ACTIVE: return "active";
    default: return "-";
    }
}

mg_linkpol_params_t mg_linkpol_params(const mg_linkpol_t *p, mg_linkpol_mode_t mode)
{
    switch (mode) {
    case MG_LINKPOL_ACTIVE:
        return (mg_linkpol_params_t){ p->itvl, p->itvl, MG_LINKPOL_LATENCY_ACTIVE, MG_LINKPOL_TIMEOUT };
    case MG_LINKPOL_SLEEP:
        if (p->sleep_tuned) {
            return p->sleep_set;   /* the bench knob (link.sleep=) */
        }
        return (mg_linkpol_params_t){ MG_LINKPOL_SLEEP_ITVL_MIN, MG_LINKPOL_SLEEP_ITVL_MAX, MG_LINKPOL_LATENCY_SLEEP,
                                      MG_LINKPOL_SLEEP_TIMEOUT };
    default:
        return (mg_linkpol_params_t){ MG_LINKPOL_READY_ITVL_MIN, MG_LINKPOL_READY_ITVL_MAX, p->ready_latency,
                                      MG_LINKPOL_TIMEOUT };
    }
}

void mg_linkpol_link_params(const mg_linkpol_t *p, uint8_t out[6])
{
    out[0] = (uint8_t)p->cur_itvl;
    out[1] = (uint8_t)(p->cur_itvl >> 8);
    out[2] = (uint8_t)p->cur_latency;
    out[3] = (uint8_t)(p->cur_latency >> 8);
    out[4] = (uint8_t)p->cur_timeout;
    out[5] = (uint8_t)(p->cur_timeout >> 8);
}

/* The link in effect satisfies the set w. */
static bool satisfied(const mg_linkpol_t *p, const mg_linkpol_params_t *w)
{
    return p->cur_itvl >= w->itvl_min && p->cur_itvl <= w->itvl_max && p->cur_latency == w->latency &&
           (!p->cur_timeout || p->cur_timeout == w->timeout);
}

static bool same_set(const mg_linkpol_params_t *a, const mg_linkpol_params_t *b)
{
    return a->itvl_min == b->itvl_min && a->itvl_max == b->itvl_max && a->latency == b->latency &&
           a->timeout == b->timeout;
}

/* A request goes now: the window opens (or, open, counts its 20 s from now) while the link in effect has a
 * latency to ignore and the bench switch is on. A re-ask of an unanswered request (2 s, up to 45 s) is the
 * same switch: it neither reopens nor extends the window, which stays capped at 20 s from the switch's
 * first request. */
static void wake_open(mg_linkpol_t *p, int64_t now_us, bool reask)
{
    if (p->wake_off || !p->cur_latency || reask) {
        return;
    }
    if (!p->wake) {
        p->wake = true;
        p->wake_epoch++;
        p->wake_since_us = now_us;
    }
    p->wake_mode = p->mode;
    p->wake_req_us = now_us;
}

static void wake_close(mg_linkpol_t *p, int64_t now_us, const char *why)
{
    if (!p->wake) {
        return;
    }
    p->wake = false;
    p->wake_why = why;
    p->wake_held_us = now_us - p->wake_since_us;
}

void mg_linkpol_set_wake(mg_linkpol_t *p, bool on, int64_t now_us)
{
    p->wake_off = !on;
    if (!on) {
        wake_close(p, now_us, "link.wake=0");
    }
}

mg_linkpol_wake_act_t mg_linkpol_wake_sync(const mg_linkpol_t *p, mg_linkpol_wake_t *w)
{
    if (p->on && p->wake) {
        if (!w->applied) {
            w->applied = true;
            w->epoch = p->wake_epoch;
            return MG_LINKPOL_WAKE_APPLY;
        }
        if (w->epoch != p->wake_epoch) {
            w->epoch = p->wake_epoch;
            return MG_LINKPOL_WAKE_REAPPLY;
        }
        return MG_LINKPOL_WAKE_KEEP;
    }
    if (w->applied) {
        w->applied = false;
        return MG_LINKPOL_WAKE_RELEASE;
    }
    return MG_LINKPOL_WAKE_KEEP;
}

uint32_t mg_linkpol_wake_period_us(uint16_t itvl)
{
    uint32_t half = (uint32_t)itvl * 1250u / 2u;
    return half > MG_LINKPOL_WAKE_PERIOD_MIN_US ? half : MG_LINKPOL_WAKE_PERIOD_MIN_US;
}

static void set_mode(mg_linkpol_t *p, int64_t now_us, mg_linkpol_mode_t m, const char *why)
{
    if (m == p->mode) {
        return;
    }
    p->mode = m;
    p->mode_changed = true;
    p->why = why;
    if (m == MG_LINKPOL_READY) {
        p->ready_since_us = now_us;
    }
    p->owed = true;   /* a real update: asked unless the link is already in the set */
    p->owed_why = NULL;
    p->refusals = 0;
    p->retry_at_us = 0;
    p->collisions = 0;
    p->collision_retry = false;
    p->busy_retry = false;
    p->unanswered_us = 0;   /* the new mode's requests get their own 45 s */
    if (p->toggle != MG_LINKPOL_TOGGLE_NONE) {
        p->note = "stream start toggle superseded by a mode change";
        p->toggle = MG_LINKPOL_TOGGLE_NONE;
    }
}

/* When the app's active write expires (0: it isn't active). */
static int64_t app_active_until(const mg_linkpol_t *p)
{
    if (p->app != MG_LINKPOL_ACTIVE) {
        return 0;
    }
    int64_t since = p->app_write_us;
    if (p->had_activity && p->last_activity_us > since) {
        since = p->last_activity_us;
    }
    return since + MG_LINKPOL_APP_ACTIVE_US;
}

/* The effective mode from what's known now. */
static void evaluate(mg_linkpol_t *p, int64_t now_us)
{
    if (!p->on) {
        return;
    }
    const char *hint = p->hint;
    p->hint = NULL;
    if (p->app == MG_LINKPOL_ACTIVE && !p->busy && now_us >= app_active_until(p)) {
        p->app = MG_LINKPOL_READY;
        p->note = "the app's active expired (30 s without activity): ready";
        hint = "the app's active expired";
    }
    bool hold = p->had_activity && now_us - p->last_activity_us < MG_LINKPOL_HOLD_US;
    if (p->busy || hold || p->turn) {
        const char *why = p->activity_why ? p->activity_why : "activity";
        if (!p->busy && !hold) {
            /* The gadget's own turn alone (mgcommands.h): its reply is still to come. */
            why = "turn: waiting for the reply";
            if (!p->turn_kept) {
                p->turn_kept = true;
                if (p->mode == MG_LINKPOL_ACTIVE) {
                    p->mode_note = "active kept (turn: waiting for the reply)";
                }
            }
        }
        set_mode(p, now_us, MG_LINKPOL_ACTIVE, why);
        return;
    }
    const char *back = hint ? hint : p->mode == MG_LINKPOL_ACTIVE ? "5 s after the last activity" : "app";
    if (p->app == MG_LINKPOL_ACTIVE) {
        set_mode(p, now_us, MG_LINKPOL_ACTIVE, "app");
    } else if (p->app == MG_LINKPOL_SLEEP) {
        /* A user's wake or touch lifts the app's sleep to ready for 10 min. */
        if (p->user_lift && now_us - p->user_us < MG_LINKPOL_SLEEP_AFTER_US) {
            set_mode(p, now_us, MG_LINKPOL_READY, back);
        } else {
            if (p->user_lift) {
                p->user_lift = false;
                back = "10 min in ready";
            }
            set_mode(p, now_us, MG_LINKPOL_SLEEP, back);
        }
    } else if (p->auto_sleep) {
        set_mode(p, now_us, MG_LINKPOL_SLEEP, "10 min in ready");
    } else if (p->mode == MG_LINKPOL_READY && now_us - p->ready_since_us >= MG_LINKPOL_SLEEP_AFTER_US) {
        p->auto_sleep = true;
        set_mode(p, now_us, MG_LINKPOL_SLEEP, "10 min in ready");
    } else {
        set_mode(p, now_us, MG_LINKPOL_READY, back);
    }
}

bool mg_linkpol_take_change(mg_linkpol_t *p, uint8_t *from, mg_linkpol_mode_t *to, const char **why)
{
    if (!p->mode_changed) {
        return false;
    }
    p->mode_changed = false;
    uint8_t was = p->reported;
    p->reported = (uint8_t)p->mode;
    if (was == (uint8_t)p->mode) {
        return false;   /* there and back between two reports: nothing the app missed */
    }
    *from = was;
    *to = p->mode;
    *why = p->why ? p->why : "-";
    return true;
}

/* A request of ours was refused (an L2CAP reject, a failed update, or the
 * stack wouldn't send it): once more after 5 s, then not until the next
 * mode change. */
static void refused(mg_linkpol_t *p, int64_t now_us)
{
    p->collision_retry = false;
    p->busy_retry = false;
    if (p->refusals == 0) {
        p->refusals = 1;
        p->retry_at_us = now_us + MG_LINKPOL_RETRY_US;
        p->note = "request refused: asking once more in 5 s";
    } else {
        p->refusals = 2;
        p->retry_at_us = 0;
        p->note = "request refused again: the link stays as it is until the next mode change";
    }
}

void mg_linkpol_conn_update(mg_linkpol_t *p, int64_t now_us, int status, uint16_t itvl, uint16_t latency,
                            uint16_t timeout)
{
    bool ours = p->outcome_owed;
    if (p->wake) {
        /* Our request's outcome closes the window; the central's own update keeps it open (our request is
         * still out) and has the caller apply its override again, unless the link now has no latency. */
        if (ours) {
            wake_close(p, now_us, status ? "refused" : "granted");
        } else if (status == 0 && latency == 0) {
            wake_close(p, now_us, "the central's own update, latency 0");
        } else {
            p->wake_epoch++;
        }
    }
    p->updated_once = true;
    p->last_update_us = now_us;   /* the next request waits MG_LINKPOL_SETTLE_US from here */
    p->pending = false;
    p->outcome_owed = false;
    p->unanswered_us = 0;   /* an answer came (ours or the central's own) */
    p->busy_retry = false;
    p->cur_itvl = itvl;
    p->cur_latency = latency;
    p->cur_timeout = timeout;
    if (p->toggle == MG_LINKPOL_TOGGLE_IDLE_ASKED) {
        /* The idle half's outcome, granted or refused: the fast half follows,
         * settled and paced. */
        p->toggle = MG_LINKPOL_TOGGLE_FAST_OWED;
        p->note = status ? "stream start toggle: the idle half refused, the fast half after 500 ms"
                         : "stream start toggle: the idle half granted, the fast half after 500 ms";
        return;
    }
    if (!ours || !p->on) {
        return;   /* the central's own update: accepted as it is */
    }
    if (status) {
        if (mg_linkpol_collision(status) && p->collisions < MG_LINKPOL_COLLISION_TRIES) {
            /* A procedure collision (LL 0x2A: our request crossed another LL procedure, the central's own
             * update): transient, so asked again after 300 ms unless the link is in the set by then. */
            p->collisions++;
            p->collision_retry = true;
            p->refused_us = now_us;
            p->retry_at_us = now_us + MG_LINKPOL_COLLISION_US;
            p->note = "request refused (LL 0x2A, a procedure collision): asking again in 300 ms";
        } else {
            refused(p, now_us);
        }
        return;
    }
    /* A sleep request's grant, for link.sleep? (what the central chose). */
    mg_linkpol_params_t sleep_set = mg_linkpol_params(p, MG_LINKPOL_SLEEP);
    if (p->mode == MG_LINKPOL_SLEEP && p->asked_valid && same_set(&p->asked, &sleep_set)) {
        p->sleep_grant_valid = true;
        p->sleep_grant_itvl = itvl;
        p->sleep_grant_latency = latency;
        p->sleep_grant_timeout = timeout;
        p->sleep_grant_us = now_us;
    }
    /* Granted. A request for another set than the mode's now (the mode
     * changed while it was out, or a toggle's idle half granted after its
     * skip) leaves the mode's set owed; what the central chose for the
     * mode's own request is accepted as it is. */
    mg_linkpol_params_t want = mg_linkpol_params(p, p->mode);
    if (p->asked_valid && !same_set(&p->asked, &want) && !satisfied(p, &want)) {
        p->owed = true;
        p->refusals = 0;
        p->owed_why = "the link isn't in the mode's set";
    }
}

bool mg_linkpol_collision(int status)
{
    return status == MG_LINKPOL_HCI_COLLISION || status == MG_LINKPOL_HS_HCI_BASE + MG_LINKPOL_HCI_COLLISION;
}

bool mg_linkpol_link_active(const mg_linkpol_t *p)
{
    return p->on && p->cur_itvl && p->cur_itvl <= p->itvl && p->cur_latency == MG_LINKPOL_LATENCY_ACTIVE;
}

void mg_linkpol_request_failed(mg_linkpol_t *p, int64_t now_us)
{
    wake_close(p, now_us, "not sent");
    p->pending = false;
    p->outcome_owed = false;
    if (p->toggle == MG_LINKPOL_TOGGLE_IDLE_ASKED) {
        p->toggle = MG_LINKPOL_TOGGLE_NONE;   /* the link never left fast: nothing to come back from */
        p->note = "stream start toggle skipped: the idle half couldn't be sent, the fast set kept";
        return;
    }
    refused(p, now_us);
}

void mg_linkpol_request_busy(mg_linkpol_t *p, int64_t now_us)
{
    if (p->toggle == MG_LINKPOL_TOGGLE_IDLE_ASKED) {
        mg_linkpol_request_failed(p, now_us);   /* the toggle is skipped, as when it can't be sent */
        return;
    }
    /* Our earlier request is still the stack's: its outcome, if it ever
     * comes, is still ours (outcome_owed stays). */
    p->pending = false;
    if (!p->unanswered_us) {
        p->unanswered_us = now_us;
    }
    if (now_us - p->unanswered_us >= MG_LINKPOL_UNANSWERED_MAX_US) {
        refused(p, now_us);
        p->note = "no answer for 45 s and the stack still holds the request: treated as refused";
        return;
    }
    p->busy_retry = true;
    p->collision_retry = false;
    p->retry_at_us = now_us + MG_LINKPOL_ANSWER_US;
    p->note = "the stack still holds the unanswered request: asking again in 2 s";
}

void mg_linkpol_disconnect(mg_linkpol_t *p)
{
    /* No clock here: the window's length is counted to its last request. */
    wake_close(p, p->wake ? p->wake_req_us : 0, "disconnect");
    p->on = false;
    p->toggle = MG_LINKPOL_TOGGLE_NONE;
    p->pending = false;
    p->outcome_owed = false;
    p->owed = false;
    p->retry_at_us = 0;
    p->collision_retry = false;
    p->busy_retry = false;
    p->unanswered_us = 0;
    p->turn = false;
    p->next_us = 0;
    p->mode_changed = false;
}

static void activity(mg_linkpol_t *p, int64_t now_us, const char *why)
{
    if (p->mode != MG_LINKPOL_ACTIVE || !p->activity_why) {
        p->activity_why = why;
    }
    p->had_activity = true;
    if (now_us > p->last_activity_us) {
        p->last_activity_us = now_us;
    }
    p->auto_sleep = false;
}

void mg_linkpol_action(mg_linkpol_t *p, int64_t now_us, const char *why)
{
    if (!p->on) {
        return;
    }
    activity(p, now_us, why);
    evaluate(p, now_us);
}

/* The turn is over: its activity ends now, and the 5 s hold counts from here. */
static void turn_over(mg_linkpol_t *p, int64_t now_us, const char *note)
{
    p->turn = false;
    p->had_activity = true;
    if (now_us > p->last_activity_us) {
        p->last_activity_us = now_us;
    }
    p->note = note;
}

void mg_linkpol_turn(mg_linkpol_t *p, int64_t now_us)
{
    if (!p->on) {
        return;
    }
    activity(p, now_us, "talk press");
    p->turn = true;
    p->turn_reply = false;   /* a reply still playing at the press isn't this turn's: it must rise again */
    p->turn_kept = false;
    p->turn_wait_us = now_us;
    evaluate(p, now_us);
}

void mg_linkpol_turn_end(mg_linkpol_t *p, int64_t now_us, bool error)
{
    if (!p->on || !p->turn) {
        return;
    }
    turn_over(p, now_us, error ? "turn over: assistant_state error" : "turn over: assistant_state done");
}

void mg_linkpol_stream_start(mg_linkpol_t *p, int64_t now_us, const char *why)
{
    if (!p->on) {
        return;
    }
    bool was_active = p->mode == MG_LINKPOL_ACTIVE;
    mg_linkpol_action(p, now_us, why);
    if (was_active && p->toggle == MG_LINKPOL_TOGGLE_NONE) {
        p->toggle = MG_LINKPOL_TOGGLE_IDLE_OWED;   /* already active: idle then fast again, two real updates */
    }
}

void mg_linkpol_user(mg_linkpol_t *p, int64_t now_us, const char *why)
{
    if (!p->on) {
        return;
    }
    p->user_lift = true;
    if (now_us > p->user_us) {
        p->user_us = now_us;
    }
    p->auto_sleep = false;
    p->hint = why;   /* "link mode sleep -> ready (touch)" */
    evaluate(p, now_us);
    p->hint = NULL;
    if (p->mode == MG_LINKPOL_READY) {
        p->ready_since_us = now_us;   /* the 10 min count from the user's last wake or touch */
    }
}

bool mg_linkpol_app_mode(mg_linkpol_t *p, int64_t now_us, uint8_t mode)
{
    if (mode > MG_LINKPOL_ACTIVE) {
        return false;
    }
    p->app = (mg_linkpol_mode_t)mode;
    p->app_write_us = now_us;
    p->auto_sleep = false;
    p->user_lift = false;   /* the app's write rules until the user's next wake or touch */
    p->hint = "app";
    evaluate(p, now_us);
    p->hint = NULL;
    return true;
}

/* The toggle's idle half: 24/36/0/400, or above the fast interval when a bench
 * moved it into that range (a range holding the live interval at the same
 * latency is rejected). */
static mg_linkpol_params_t toggle_idle(const mg_linkpol_t *p)
{
    uint16_t lo = MG_LINKPOL_TOGGLE_ITVL_MIN, hi = MG_LINKPOL_TOGGLE_ITVL_MAX;
    if (p->itvl >= lo && p->itvl <= hi) {
        lo = (uint16_t)(p->itvl + 12);
        hi = (uint16_t)(p->itvl + 24);
    }
    return (mg_linkpol_params_t){ lo, hi, MG_LINKPOL_TOGGLE_LATENCY, MG_LINKPOL_TIMEOUT };
}

static void wake_at(mg_linkpol_t *p, int64_t t)
{
    if (!p->next_us || t < p->next_us) {
        p->next_us = t;
    }
}

bool mg_linkpol_step(mg_linkpol_t *p, int64_t now_us, const mg_linkpol_in_t *in, mg_linkpol_params_t *out)
{
    p->next_us = 0;
    if (!p->on) {
        return false;
    }
    p->busy = in->busy != NULL;
    if (p->busy) {
        activity(p, now_us, in->busy);   /* anything ongoing holds it active, and 5 s after it ends */
    }
    /* The gadget's own turn: over once the reply that rose after its press stops, or 20 s on with no
     * reply (counted from the last round something ongoing ran: the press's capture). */
    if (p->turn) {
        if (in->reply && !p->reply) {
            p->turn_reply = true;
        }
        if (p->turn_reply && !in->reply) {
            turn_over(p, now_us, "turn over: its reply played");
        } else if (!p->turn_reply) {
            if (p->busy) {
                p->turn_wait_us = now_us;
            }
            if (now_us - p->turn_wait_us >= MG_LINKPOL_TURN_WAIT_US) {
                turn_over(p, now_us, "turn over: 20 s with no reply");
            } else {
                wake_at(p, p->turn_wait_us + MG_LINKPOL_TURN_WAIT_US);
            }
        }
    }
    p->reply = in->reply;
    evaluate(p, now_us);

    /* The rounds the mode needs by itself: the 5 s hold, the app's 30 s, the
     * 10 min (in ready, or of a user's lift of the app's sleep). */
    if (!p->busy && p->had_activity && now_us - p->last_activity_us < MG_LINKPOL_HOLD_US) {
        wake_at(p, p->last_activity_us + MG_LINKPOL_HOLD_US);
    }
    if (!p->busy && p->app == MG_LINKPOL_ACTIVE) {
        wake_at(p, app_active_until(p));
    }
    if (p->mode == MG_LINKPOL_READY && p->app == MG_LINKPOL_READY && !p->auto_sleep) {
        wake_at(p, p->ready_since_us + MG_LINKPOL_SLEEP_AFTER_US);
    }
    if (p->mode == MG_LINKPOL_READY && p->app == MG_LINKPOL_SLEEP && p->user_lift) {
        wake_at(p, p->user_us + MG_LINKPOL_SLEEP_AFTER_US);
    }

    /* A request with no outcome in 2 s: unanswered. The toggle's idle half
     * then skips the toggle for this stream: nothing is stacked on
     * it, the fast set stays; a late grant that leaves the link slower is
     * asked back. */
    if (p->pending && now_us - p->last_request_us >= MG_LINKPOL_ANSWER_US) {
        p->pending = false;
        if (p->toggle == MG_LINKPOL_TOGGLE_IDLE_ASKED) {
            p->toggle = MG_LINKPOL_TOGGLE_NONE;
            p->note = "stream start toggle skipped: no answer to the idle half in 2 s, the fast set kept";
        } else {
            /* No answer at all: asked again now, unless the link
             * is in the mode's set by then; within 45 s of the mode's first
             * unanswered request, then as a refusal. */
            mg_linkpol_params_t mode_set = mg_linkpol_params(p, p->mode);
            p->unanswered++;
            if (!p->unanswered_us) {
                p->unanswered_us = p->last_request_us;
            }
            if (satisfied(p, &mode_set)) {
                p->unanswered_us = 0;
            } else if (now_us - p->unanswered_us >= MG_LINKPOL_UNANSWERED_MAX_US) {
                refused(p, now_us);
                p->note = "no answer to the request for 45 s: treated as refused";
            } else {
                p->owed = true;
                p->owed_why = "no answer in 2 s";
                p->retry_at_us = 0;
                p->note = "no answer to the request in 2 s, asking again";
            }
        }
    }
    /* The listening window: 20 s after the switch's request at most (its re-asks don't extend it). */
    if (p->wake) {
        if (now_us - p->wake_req_us >= MG_LINKPOL_WAKE_US) {
            wake_close(p, now_us, "20 s");
        } else {
            wake_at(p, p->wake_req_us + MG_LINKPOL_WAKE_US);
        }
    }
    if (p->retry_at_us) {
        if (now_us >= p->retry_at_us) {
            p->retry_at_us = 0;
            p->owed = true;
            p->owed_why = p->collision_retry ? "again after a collision"
                          : p->busy_retry    ? "again: the stack held the unanswered request"
                                             : "once more after a refusal";
            p->busy_retry = false;
        } else {
            wake_at(p, p->retry_at_us);
        }
    }

    mg_linkpol_params_t want = mg_linkpol_params(p, p->mode);
    bool force = false;
    switch (p->toggle) {
    case MG_LINKPOL_TOGGLE_IDLE_OWED:
        want = toggle_idle(p);
        force = true;
        break;
    case MG_LINKPOL_TOGGLE_IDLE_ASKED:
        wake_at(p, p->last_request_us + MG_LINKPOL_ANSWER_US);   /* its outcome, or the skip */
        return false;
    case MG_LINKPOL_TOGGLE_FAST_OWED:
        force = true;   /* even if the idle half was refused: the fast half still goes */
        break;
    default:
        break;
    }
    if (!force) {
        if (!p->owed) {
            return false;
        }
        if (satisfied(p, &want)) {
            p->owed = false;   /* never re-ask the set the link is in (a central rejects it) */
            p->collision_retry = false;
            return false;
        }
    }
    /* Owed: not before the central's first update or 1 s after connect, never
     * on an unanswered request, at most one per 2 s, and never within 500 ms
     * of a connection update. */
    if (!p->updated_once && now_us - p->connect_us < MG_LINKPOL_CONNECT_WAIT_US) {
        wake_at(p, p->connect_us + MG_LINKPOL_CONNECT_WAIT_US);
        return false;
    }
    if (p->pending) {
        wake_at(p, p->last_request_us + MG_LINKPOL_ANSWER_US);
        return false;
    }
    /* A collision's retry goes 300 ms after the refusal, outside the 2 s pacing and the refusal's own
     * settle; an update after the refusal (the central's own) still settles it. */
    bool quick = p->collision_retry && p->toggle == MG_LINKPOL_TOGGLE_NONE;
    if (!quick && p->requested_once && now_us - p->last_request_us < MG_LINKPOL_GAP_US) {
        wake_at(p, p->last_request_us + MG_LINKPOL_GAP_US);
        return false;
    }
    if (p->updated_once && now_us - p->last_update_us < MG_LINKPOL_SETTLE_US &&
        !(quick && p->last_update_us == p->refused_us)) {
        wake_at(p, p->last_update_us + MG_LINKPOL_SETTLE_US);
        return false;
    }
    if (p->toggle == MG_LINKPOL_TOGGLE_IDLE_OWED) {
        p->toggle = MG_LINKPOL_TOGGLE_IDLE_ASKED;
        p->req_why = "stream start while active: idle, then fast";
    } else if (p->toggle == MG_LINKPOL_TOGGLE_FAST_OWED) {
        p->toggle = MG_LINKPOL_TOGGLE_NONE;
        p->req_why = "stream start: fast again";
    } else {
        p->req_why = p->owed_why ? p->owed_why : p->why;
    }
    p->owed_why = NULL;
    p->collision_retry = false;
    p->requested_once = true;
    p->last_request_us = now_us;
    p->pending = true;
    p->outcome_owed = true;
    p->owed = false;
    p->asked = want;
    p->asked_valid = true;
    wake_at(p, now_us + MG_LINKPOL_ANSWER_US);   /* its outcome, or no answer */
    wake_open(p, now_us, p->unanswered_us != 0);   /* unanswered_us: this switch's request went unanswered */
    if (p->wake) {
        wake_at(p, p->wake_req_us + MG_LINKPOL_WAKE_US);
    }
    *out = want;
    return true;
}

/* ---- the sleep set's bench knob (link.sleep=, the sleep-parameter A/B) ---- */

bool mg_linkpol_sleep_ok(uint16_t itvl_min, uint16_t itvl_max, uint16_t latency, uint16_t timeout)
{
    /* timeout x 10 ms > (1 + latency) x itvl_max x 1.25 ms x 2  <=>  4 x timeout > (1 + latency) x itvl_max */
    return itvl_min >= 6 && itvl_max <= 3200 && itvl_min <= itvl_max && latency <= 499 && timeout >= 10 &&
           timeout <= 3200 && 4u * timeout > (uint32_t)(1u + latency) * itvl_max;
}

bool mg_linkpol_apple_ok(const mg_linkpol_params_t *s)
{
    /* Interval >= 15 ms, latency <= 30, timeout 2-6 s, max x (latency + 1) <= 2 s, timeout > 3 x max x
     * (latency + 1), max >= min + 15 ms unless both are 15 ms. In units:
     * itvl_max x (latency + 1) <= 1600, 8 x timeout > 3 x itvl_max x (latency + 1). */
    uint32_t span = (uint32_t)s->itvl_max * (1u + s->latency);
    bool step = (s->itvl_min == 12 && s->itvl_max == 12) || s->itvl_max >= s->itvl_min + 12;
    return s->itvl_min >= 12 && s->latency <= 30 && s->timeout >= 200 && s->timeout <= 600 && span <= 1600 &&
           8u * s->timeout > 3u * span && step;
}

/* A change to the sleep set while the link is in sleep: the new set is owed now (paced as any request). */
static void sleep_set_changed(mg_linkpol_t *p)
{
    if (!p->on || p->mode != MG_LINKPOL_SLEEP) {
        return;
    }
    p->owed = true;
    p->owed_why = "link.sleep";
    p->refusals = 0;
    p->retry_at_us = 0;
    p->collisions = 0;
    p->collision_retry = false;
    p->busy_retry = false;
    p->unanswered_us = 0;
}

bool mg_linkpol_set_sleep(mg_linkpol_t *p, uint16_t itvl_min, uint16_t itvl_max, uint16_t latency)
{
    if (!mg_linkpol_sleep_ok(itvl_min, itvl_max, latency, MG_LINKPOL_SLEEP_TIMEOUT)) {
        return false;
    }
    p->sleep_tuned = true;
    p->sleep_set = (mg_linkpol_params_t){ itvl_min, itvl_max, latency, MG_LINKPOL_SLEEP_TIMEOUT };
    sleep_set_changed(p);
    return true;
}

void mg_linkpol_sleep_default(mg_linkpol_t *p)
{
    p->sleep_tuned = false;
    p->sleep_set = (mg_linkpol_params_t){ 0 };
    sleep_set_changed(p);
}

/* A decimal field of up to 5 digits ending at stop; advances *s past stop. */
static bool sleep_field(const char **s, char stop, uint32_t *v)
{
    const char *c = *s;
    uint32_t x = 0;
    int n = 0;
    for (; *c >= '0' && *c <= '9'; c++) {
        if (++n > 5) {
            return false;
        }
        x = x * 10 + (uint32_t)(*c - '0');
    }
    if (!n || *c != stop) {
        return false;
    }
    *s = stop ? c + 1 : c;
    *v = x;
    return true;
}

/* 1.25 ms units as "125.00" (hundredths of a millisecond). */
static void ms_text(char *b, size_t n, uint16_t itvl)
{
    unsigned c = (unsigned)itvl * 125u;
    snprintf(b, n, "%u.%02u", c / 100u, c % 100u);
}

static void sleep_report(const mg_linkpol_t *p, int64_t now_us, const char *head, char *out, size_t n)
{
    mg_linkpol_params_t s = mg_linkpol_params(p, MG_LINKPOL_SLEEP);
    char lo[12], hi[12], g[12];
    ms_text(lo, sizeof(lo), s.itvl_min);
    ms_text(hi, sizeof(hi), s.itvl_max);
    int k = snprintf(out, n, "%s %u-%u/%u/%u (%s; %s-%s ms, latency %u, %u ms timeout%s)", head, s.itvl_min,
                     s.itvl_max, s.latency, s.timeout, p->sleep_tuned ? "link.sleep" : "default", lo, hi, s.latency,
                     s.timeout * 10u, mg_linkpol_apple_ok(&s) ? "" : "; outside Apple's rules, an iPhone may refuse it");
    if (k < 0 || (size_t)k >= n) {
        return;
    }
    if (!p->sleep_grant_valid) {
        snprintf(out + k, n - (size_t)k, ", no sleep grant yet");
        return;
    }
    ms_text(g, sizeof(g), p->sleep_grant_itvl);
    long long ago = (long long)((now_us - p->sleep_grant_us) / 1000000);
    snprintf(out + k, n - (size_t)k, ", last sleep grant %u/%u/%u (%s ms, latency %u) %lld s ago", p->sleep_grant_itvl,
             p->sleep_grant_latency, p->sleep_grant_timeout, g, p->sleep_grant_latency, ago < 0 ? 0 : ago);
}

bool mg_linkpol_sleep_word(mg_linkpol_t *p, const char *line, int64_t now_us, char *out, size_t n)
{
    if (strncmp(line, "link.sleep", 10)) {
        return false;
    }
    const char *a = line + 10;
    if (!strcmp(a, "?")) {
        sleep_report(p, now_us, "link.sleep", out, n);
        return true;
    }
    if (!strcmp(a, "=0")) {
        mg_linkpol_sleep_default(p);
        sleep_report(p, now_us, "link.sleep", out, n);
        return true;
    }
    uint32_t lo, hi, lat;
    const char *c = a + 1;
    if (a[0] == '=' && sleep_field(&c, ',', &lo) && sleep_field(&c, ',', &hi) && sleep_field(&c, 0, &lat) &&
        lo <= 0xFFFF && hi <= 0xFFFF && lat <= 0xFFFF &&
        mg_linkpol_set_sleep(p, (uint16_t)lo, (uint16_t)hi, (uint16_t)lat)) {
        sleep_report(p, now_us, "link.sleep", out, n);
        return true;
    }
    snprintf(out, n,
             "link.sleep: link.sleep=ITVL_MIN,ITVL_MAX,LAT (1.25 ms units, 6-3200, min <= max; latency 0-499; "
             "%u ms timeout > (1 + LAT) x ITVL_MAX x 2), link.sleep=0 (defaults) or link.sleep?",
             MG_LINKPOL_SLEEP_TIMEOUT * 10u);
    return true;
}
