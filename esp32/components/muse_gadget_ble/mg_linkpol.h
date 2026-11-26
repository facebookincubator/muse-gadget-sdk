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

/*
 * Link modes (mgcommands.h "Link modes"): several gadgets share one phone's radio, and the central
 * serves every link at its interval whatever the peripheral latency, so only
 * the gadget in use holds a short interval. Three modes, each a set of
 * connection parameters (interval min/max in 1.25 ms units, latency, timeout
 * in 10 ms units; all meet Apple's accessory rules):
 *
 *   sleep  (0)  100/160/4/600   125-200 ms, latency 4, 6 s
 *   ready  (1)   24/36/5/400     30-45 ms,  latency 5, 4 s
 *   active (2)   12/12/0/400     15 ms,     latency 0, 4 s
 *
 * - Ready at every connect. Its request goes after the central's own first
 *   update, or 1 s after connect if none came (MG_LINKPOL_CONNECT_WAIT_US):
 *   a request colliding with the central's own update can go unanswered and
 *   the controller then drops the link on its 40 s LL procedure timer.
 * - Active on the device's own activity, never slower than before: its talk
 *   press (at touch-down), a stream the app starts (video, UI, audio playback), push-to-talk capture, Link setup and
 *   an MCUmgr update for their whole duration. mg_linkpol_action() marks an
 *   instant (a press); mg_linkpol_in_t.busy something ongoing.
 * - The gadget's own turn is activity (mgcommands.h;
 *   on the Watcher, without it, the policy dropped to ready 5 s after the
 *   release while Muse was still thinking, and the reply started 8 s later on
 *   the ready link): from its talk press (the press the client heard,
 *   mg_linkpol_turn()) until its reply has played (a reply stream that
 *   started after the press stopped playing: mg_linkpol_in_t.reply), or
 *   assistant_state done or error (mg_linkpol_turn_end()), or 20 s with no
 *   reply (MG_LINKPOL_TURN_WAIT_US, counted from the last round something
 *   ongoing ran: the press's capture). Then the 5 s below, from the turn's
 *   end. Logged once a turn holds it past the 5 s: `link mode active kept
 *   (turn: waiting for the reply)` (mode_note), and as the mode change's
 *   reason if the turn is what makes it active.
 * - 5 s after the last activity ends (MG_LINKPOL_HOLD_US) it returns to the
 *   app's mode: ready unless the app wrote sleep or active. After 10 min in
 *   ready (MG_LINKPOL_SLEEP_AFTER_US) it goes to sleep by itself, until the
 *   next activity, the user's next wake or touch, or the app's next write.
 * - A user's wake or touch on the gadget (any key press, a touch, the screen
 *   lit by the user; the bench's injected input alike: automation behaves
 *   like a user) moves sleep to ready, never active, and restarts the 10 min
 *   (mgcommands.h; from sleep a gadget with an S22 waited 11.75 s
 *   for the central to schedule the active update, 1.5-3 s from ready). In
 *   ready it restarts the 10 min the same way. A sleep the app wrote is
 *   lifted to ready too, for 10 min after the user's last wake or touch,
 *   then sleep again (the app's next write rules again). mg_linkpol_user();
 *   a talk press is still activity (active at once, mg_linkpol_action()),
 *   and a user's wake while active only restarts the count.
 * - The app's mode (setting link_mode 16, per connection): a write of active
 *   makes it active ahead of a stream, and expires to ready after 30 s
 *   without activity (MG_LINKPOL_APP_ACTIVE_US), so an app that disappears
 *   can't pin it active; a write of ready or sleep takes effect once the
 *   device's own activity has ended. A read returns the effective mode
 *   (mode), and every effective change is reported (mg_linkpol_take_change)
 *   so the caller notifies the setting.
 * - Each mode change asks its set once, unless the link already satisfies
 *   it (a request for the set the link is in is rejected; never re-sent). It
 *   accepts any update the central makes by itself. A refused request (an
 *   L2CAP reject, a failed update, or the stack refusing to send it) leaves
 *   the link as it is and is asked once more after 5 s
 *   (MG_LINKPOL_RETRY_US), then left until the next mode change. A refusal
 *   for a procedure collision (LL error 0x2A, Different Transaction
 *   Collision: NimBLE's status 554, BLE_HS_ERR_HCI_BASE + 0x2A; on the Watcher,
 *   our active request crossed the central's own update) is asked
 *   again after 300 ms (MG_LINKPOL_COLLISION_US) instead, unless the link is
 *   already in the set by then (the central's own update may have put it
 *   there), outside the 2 s pacing and the refusal's own 500 ms settle (a
 *   later update still settles it); up to MG_LINKPOL_COLLISION_TRIES times a
 *   mode, then as any refusal.
 * - A video stream that starts while already active gets a brief
 *   idle -> fast toggle so the central re-plans the link (the
 *   iPhone's two-gadget starvation: re-asking identical parameters is a
 *   no-op):
 *   1. the idle half, MG_LINKPOL_TOGGLE_* = 24/36/0/400 (it excludes the fast
 *      15 ms by interval alone; latency 0, so a peripheral sleeping through
 *      latency can't miss the next instant);
 *   2. its outcome: the connection update event, granted or refused;
 *   3. the fast half no sooner than 500 ms after that outcome
 *      (MG_LINKPOL_SETTLE_US), and no sooner than 2 s after the idle half
 *      (the 2 s rule below), whichever is later. A gadget with an S22: a fast
 *      half asked 1 ms after the idle half's grant went silent at its
 *      instant and the link dropped on supervision timeout.
 *   No outcome within 2 s (MG_LINKPOL_ANSWER_US): the toggle is skipped for
 *   that stream (logged), nothing more is asked and the fast set stays. A
 *   mode change in between supersedes it; a disconnect cancels it; another
 *   stream start during it joins it.
 * - A request with no outcome in 2 s (MG_LINKPOL_ANSWER_US: no grant, no
 *   refusal) is asked again then, unless the link is in the mode's set by
 *   then (on the Watcher a talk press from sleep asked active, no
 *   answer ever came, and since only a refusal re-asked, the whole turn and
 *   its 3.3 s capture rode the 200 ms link). Logged `no answer to the request
 *   in 2 s, asking again`. NimBLE keeps an unanswered request for 40 s and
 *   ends it silently (no connection update event), refusing another meanwhile
 *   (BLE_HS_EALREADY): that isn't a refusal (mg_linkpol_request_busy), it's
 *   asked again 2 s later. Bounded: re-asks for one mode stop 45 s after its
 *   first unanswered request (MG_LINKPOL_UNANSWERED_MAX_US, past NimBLE's
 *   40 s), and it's then a refusal (once more after 5 s, then left until the
 *   next mode change). A late outcome still counts as the request's own.
 * - Pacing: at most one request per 2 s, never one while the last is still
 *   unanswered (up to 2 s), nor within 500 ms of any connection update (the
 *   central's own or a grant: the same instant).
 * - Screen and USB power don't matter.
 * - mg_linkpol_step() leaves in next_us the next time it needs a round (a
 *   paced, settling, awaited or retried request, the 5 s hold, the app's
 *   30 s, the 10 min); mg_ble.c wakes its worker then with a one-shot timer,
 *   never waiting on the NimBLE host task.
 *
 * - Listening at every event during a switch (a Nordic XIAO with a Mac central: sleep -> active median 6.8 ->
 *   3.4 s): from each request until its set applies (the grant), a refusal,
 *   the stack refusing to send it, the link's loss, or 20 s after the request
 *   (MG_LINKPOL_WAKE_US; the switch's request: its re-asks of an unanswered
 *   request neither extend nor reopen it), the peripheral ignores its slave latency locally,
 *   so the central's answer and its LL_CONNECTION_UPDATE_IND are heard at
 *   the next event rather than after up to latency + 1 intervals. Opened only
 *   while the link in effect has a latency (nothing to ignore at 0). The
 *   central's own update while it's open keeps it open and asks the caller to
 *   apply its override again (wake_epoch: the S3's controller drops it at
 *   every connection update instant). The bench switch link.wake=0|1 (off by
 *   default on the ESP32, CONFIG_MUSE_GADGET_BLE_LINK_WAKE; RAM only, kept across connections: wake_off) turns it off or on;
 *   off closes an open window. mg_linkpol_wake_sync() tells the caller what
 *   to do with its override; mg_ble.c holds the mechanism (the S3 and C3
 *   controllers' vendor command 0xFC13, Set Preferred Slave Latency 0; on the
 *   others a notification kept pending every half interval, since a
 *   peripheral with queued data transmits at every event: README, Link modes).
 *
 * - The sleep set's bench knob (link.sleep=ITVL_MIN,ITVL_MAX,LAT, the
 *   sleep-parameter A/B): RAM only and kept across connections like
 *   wake_off and ready_latency, it replaces MG_LINKPOL_SLEEP_ITVL_MIN/MAX and
 *   MG_LINKPOL_LATENCY_SLEEP in the sleep mode's request (the timeout stays
 *   MG_LINKPOL_SLEEP_TIMEOUT). A value the BLE spec refuses is refused
 *   (mg_linkpol_sleep_ok: interval 6-3200, min <= max, latency 0-499,
 *   timeout > (1 + latency) x max x 2); link.sleep=0 restores the defaults;
 *   link.sleep? reports the set in effect and the last sleep grant. A change
 *   while the link is in sleep asks the new set. mg_linkpol_sleep_word()
 *   handles the words; mg_ble.c and the USB console call it.
 *
 * Pure logic, no IDF: mg_ble.c feeds it, sends what it asks for, logs each
 * mode change, request and grant on mg.ble and notifies link_mode;
 * tests/test_mg_linkpol.py runs it on the host.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The modes' values are mgcommands.h's mg_link_mode_t (mg_ble.c checks). */
typedef enum {
    MG_LINKPOL_SLEEP = 0,
    MG_LINKPOL_READY = 1,
    MG_LINKPOL_ACTIVE = 2,
} mg_linkpol_mode_t;

#define MG_LINKPOL_TIMEOUT 400             /* 10 ms units: 4 s, ready and active (sleep: MG_LINKPOL_SLEEP_TIMEOUT) */
#define MG_LINKPOL_ITVL 12                 /* active: 15 ms, min = max (Apple grants 15 ms only for exactly 12/12) */
#define MG_LINKPOL_LATENCY_ACTIVE 0
#define MG_LINKPOL_READY_ITVL_MIN 24       /* ready: 30-45 ms (Apple: max >= min + 15 ms) */
#define MG_LINKPOL_READY_ITVL_MAX 36
#define MG_LINKPOL_LATENCY_READY 5
#define MG_LINKPOL_SLEEP_ITVL_MIN 100      /* sleep: 125-200 ms, latency 4, 6 s timeout (mgcommands.h):
                                              at a 200 ms grant, 6 s / (5 x 200 ms) = 6 listens of slack */
#define MG_LINKPOL_SLEEP_ITVL_MAX 160
#define MG_LINKPOL_LATENCY_SLEEP 4
#define MG_LINKPOL_SLEEP_TIMEOUT 600       /* 10 ms units: 6 s, the sleep set only */
#define MG_LINKPOL_HOLD_US (5LL * 1000 * 1000)                 /* back to the app's mode this long after the last activity */
#define MG_LINKPOL_SLEEP_AFTER_US (10LL * 60 * 1000 * 1000)    /* this long in ready: sleep by itself */
#define MG_LINKPOL_APP_ACTIVE_US (30LL * 1000 * 1000)          /* the app's active expires after this without activity */
#define MG_LINKPOL_CONNECT_WAIT_US (1000 * 1000)   /* no request this soon after connect unless the central updated */
#define MG_LINKPOL_GAP_US (2000 * 1000)    /* at most one request per 2 s */
#define MG_LINKPOL_ANSWER_US MG_LINKPOL_GAP_US   /* a request with no outcome this long: unanswered */
#define MG_LINKPOL_SETTLE_US (500 * 1000)  /* no request sooner than this after a connection update */
#define MG_LINKPOL_RETRY_US (5LL * 1000 * 1000)   /* a refused request is asked once more after this */
#define MG_LINKPOL_UNANSWERED_MAX_US (45LL * 1000 * 1000)   /* unanswered requests re-asked this long, then a refusal */
#define MG_LINKPOL_COLLISION_US (300 * 1000)      /* ... a collision's (LL 0x2A) after this */
#define MG_LINKPOL_COLLISION_TRIES 2              /* collision retries a mode, then as any refusal */
#define MG_LINKPOL_HCI_COLLISION 0x2A             /* LL error: Different Transaction Collision */
#define MG_LINKPOL_HS_HCI_BASE 0x200              /* NimBLE's status for an HCI error: this + the code (554) */
#define MG_LINKPOL_TURN_WAIT_US (20LL * 1000 * 1000)   /* a turn with no reply this long is over */
#define MG_LINKPOL_WAKE_US (20LL * 1000 * 1000)        /* a listening window closes this long after its request */
#define MG_LINKPOL_WAKE_PERIOD_MIN_US 3750              /* the pending notification's period: half the interval, at least this */
/* The stream-start toggle's idle half: excludes the fast 15 ms by
 * interval alone, latency 0. Apple: max >= min + 15 ms. */
#define MG_LINKPOL_TOGGLE_ITVL_MIN 24
#define MG_LINKPOL_TOGGLE_ITVL_MAX 36
#define MG_LINKPOL_TOGGLE_LATENCY 0

/* The toggle's state. */
enum {
    MG_LINKPOL_TOGGLE_NONE = 0,
    MG_LINKPOL_TOGGLE_IDLE_OWED,      /* the idle half is owed */
    MG_LINKPOL_TOGGLE_IDLE_ASKED,     /* the idle half is out: waiting for its outcome (up to 2 s) */
    MG_LINKPOL_TOGGLE_FAST_OWED,      /* its outcome came: the fast half, once settled and paced */
};

#define MG_LINKPOL_NO_MODE 0xFF       /* take_change's from at connect: nothing reported yet */

typedef struct {
    uint16_t itvl_min, itvl_max;   /* 1.25 ms units */
    uint16_t latency;
    uint16_t timeout;              /* 10 ms units */
} mg_linkpol_params_t;

/* What the board says each round. */
typedef struct {
    /* Something ongoing that keeps it active, for the log ("video stream",
     * "audio playback", "push-to-talk capture", "setup", ...); NULL:
     * nothing. */
    const char *busy;
    /* A reply is playing: a reply stream open (in a stream kept open,
     * audio playing). The gadget's turn ends when the reply that started
     * after its press stops. */
    bool reply;
} mg_linkpol_in_t;

typedef struct {
    bool on;                    /* connected */
    uint16_t itvl;              /* active interval (the console's bench knob; 12) */
    uint16_t ready_latency;     /* ready latency (the console's bench knob; 5) */
    mg_linkpol_mode_t mode;     /* the effective mode */
    mg_linkpol_mode_t app;      /* the app's mode (link_mode writes): ready at connect */
    bool auto_sleep;            /* 10 min in ready: asleep by itself until activity, a user's wake or a write */
    bool user_lift;             /* a user's wake or touch since the app's last write: its sleep is ready */
    int64_t user_us;            /* ...the last one (the app's sleep lifted until 10 min after it) */
    int64_t connect_us;
    int64_t app_write_us;       /* the app's last write */
    bool had_activity;
    int64_t last_activity_us;   /* the last instant of activity (a press, or a round while busy) */
    const char *activity_why;   /* the activity that made it active, for the log */
    bool busy;                  /* the last step's: something ongoing */
    int64_t ready_since_us;
    const char *hint;           /* why the next mode change happens, if the caller knows ("app") */
    uint16_t cur_itvl, cur_latency, cur_timeout;   /* in effect (link_params) */
    mg_linkpol_params_t asked;  /* the last request */
    bool asked_valid;
    bool owed;                  /* the mode's set is owed (a change, or a retry) */
    uint8_t toggle;             /* stream start while active: MG_LINKPOL_TOGGLE_* */
    bool pending;               /* the last request has had no outcome yet, up to MG_LINKPOL_ANSWER_US */
    bool outcome_owed;          /* ...nor late: a refusal still counts as its own */
    uint8_t refusals;           /* this mode's refused requests: 1 asks once more, 2 waits for the next change */
    int64_t retry_at_us;        /* the retry (0: none) */
    uint8_t collisions;         /* this mode's collision refusals (LL 0x2A), each retried after 300 ms */
    bool collision_retry;       /* the retry owed is a collision's: outside the 2 s pacing */
    int64_t refused_us;         /* the collision's refusal (its own update doesn't settle the retry) */
    int64_t unanswered_us;      /* this mode's first request left unanswered (0: none): re-asks stop 45 s on */
    bool busy_retry;            /* the retry owed is after the stack still held the unanswered request */
    uint16_t unanswered;        /* requests left unanswered since connect (diagnostics) */
    /* The gadget's own turn (mgcommands.h): its talk press until its reply has played. */
    bool turn;
    bool turn_reply;            /* the reply started (a reply rose after the press) */
    bool turn_kept;             /* the turn held it past the 5 s: logged once */
    int64_t turn_wait_us;       /* waiting for the reply since (the press, or the last busy round) */
    bool reply;                 /* the last step's in->reply */
    const char *mode_note;      /* "active kept (turn: waiting for the reply)": the caller logs "link mode %s", then clears */
    bool updated_once;
    int64_t last_update_us;     /* the last connection update event (granted or refused) */
    bool requested_once;
    int64_t last_request_us;
    int64_t next_us;            /* set by each step: when it wants the next round (0: no need) */
    const char *note;           /* for the log (toggle outcomes, refusals, the app's expiry): the caller logs, then clears */
    bool mode_changed;          /* the effective mode changed since the last take_change */
    uint8_t reported;           /* the mode the last take_change reported (MG_LINKPOL_NO_MODE at connect) */
    const char *why;            /* why the last mode change */
    const char *owed_why;       /* why the set is owed when it isn't a mode change (a retry), else NULL */
    const char *req_why;        /* why the last request */
    /* Listening at every event during a switch. */
    bool wake_off;              /* link.wake=0 (the bench switch; kept across connections) */
    bool wake;                  /* the window is open: the caller keeps the peripheral listening at every event */
    uint8_t wake_epoch;         /* bumped at each open and at the central's own update while open: (re)apply */
    mg_linkpol_mode_t wake_mode;   /* the mode the window's request switches to, for the log */
    int64_t wake_since_us;      /* the window opened (the log's N ms) */
    int64_t wake_req_us;        /* the switch's request (not its re-asks): the 20 s count from here */
    const char *wake_why;       /* why the last window closed ("granted", "refused", "not sent", "disconnect", "20 s",
                                 * "link.wake=0") */
    int64_t wake_held_us;       /* ...how long it was open */
    /* The sleep set's bench knob (link.sleep=; kept across connections). */
    bool sleep_tuned;           /* link.sleep= set one: sleep_set replaces the defaults */
    mg_linkpol_params_t sleep_set;
    /* The last grant of a sleep request (link.sleep?; kept across connections). */
    bool sleep_grant_valid;
    uint16_t sleep_grant_itvl, sleep_grant_latency, sleep_grant_timeout;
    int64_t sleep_grant_us;
} mg_linkpol_t;

/* The caller's override, as mg_linkpol_wake_sync() last left it. */
typedef struct {
    bool applied;
    uint8_t epoch;
} mg_linkpol_wake_t;

typedef enum {
    MG_LINKPOL_WAKE_KEEP = 0,   /* nothing to do */
    MG_LINKPOL_WAKE_APPLY,      /* a window opened: ignore the latency now (log "listening every event") */
    MG_LINKPOL_WAKE_REAPPLY,    /* still open, but the link changed (the central's own update): apply again */
    MG_LINKPOL_WAKE_RELEASE,    /* closed (wake_why, wake_held_us): back to the granted latency */
} mg_linkpol_wake_act_t;

void mg_linkpol_init(mg_linkpol_t *p);
/* A new connection at now_us, with the parameters it came up with: ready. */
void mg_linkpol_connect(mg_linkpol_t *p, int64_t now_us, uint16_t itvl, uint16_t latency, uint16_t timeout);
/* A connection update event at now_us (the central's own, or the outcome of a
 * request): status 0 with the parameters now in effect, else refused (the
 * link unchanged; the parameters are still the ones in effect). */
void mg_linkpol_conn_update(mg_linkpol_t *p, int64_t now_us, int status, uint16_t itvl, uint16_t latency,
                            uint16_t timeout);
/* The stack refused to send the request mg_linkpol_step() just returned. */
void mg_linkpol_request_failed(mg_linkpol_t *p, int64_t now_us);
/* The stack wouldn't send it because it still holds our earlier, unanswered
 * request (NimBLE's BLE_HS_EALREADY, for up to 40 s): not a refusal; asked
 * again in 2 s, within the unanswered bound. */
void mg_linkpol_request_busy(mg_linkpol_t *p, int64_t now_us);
/* The link dropped: whatever was owed or awaited is dropped with it. */
void mg_linkpol_disconnect(mg_linkpol_t *p);
/* The device's own activity at now_us (a talk press at touch-down, a stream's
 * start, an update's write): active now. why: a static string for the log. */
void mg_linkpol_action(mg_linkpol_t *p, int64_t now_us, const char *why);
/* A user's wake or touch on the gadget at now_us (a key press, a touch, the
 * screen lit by the user, or the bench's injected input): sleep -> ready (not
 * active), and the 10 min in ready restarted; a sleep the app wrote is
 * ready until 10 min after it. why: a static string for the log ("touch",
 * "key", "wake"). */
void mg_linkpol_user(mg_linkpol_t *p, int64_t now_us, const char *why);
/* The gadget's own turn starts at now_us (the talk press the client heard):
 * activity, held until its reply has played, done or error, or 20 s with no
 * reply. */
void mg_linkpol_turn(mg_linkpol_t *p, int64_t now_us);
/* assistant_state done or error: the turn is over; the 5 s hold counts from
 * now. */
void mg_linkpol_turn_end(mg_linkpol_t *p, int64_t now_us, bool error);
/* A refusal status for a procedure collision (LL 0x2A, raw or as NimBLE's 554). */
bool mg_linkpol_collision(int status);
/* The link in effect is active's set (15 ms, latency 0): a reply plays at the
 * normal start then, else at MG_PLAY_SLOW_START_US. */
bool mg_linkpol_link_active(const mg_linkpol_t *p);
/* A video stream starts: activity, and if the link is already active, a
 * brief idle -> fast toggle so the central re-plans it (one already under
 * way covers this start too). */
void mg_linkpol_stream_start(mg_linkpol_t *p, int64_t now_us, const char *why);

/* The app wrote link_mode: false if mode isn't one (nothing changes). */
bool mg_linkpol_app_mode(mg_linkpol_t *p, int64_t now_us, uint8_t mode);
/* Each worker round. True when a request should go now, with *out. Sets
 * p->next_us. */
bool mg_linkpol_step(mg_linkpol_t *p, int64_t now_us, const mg_linkpol_in_t *in, mg_linkpol_params_t *out);
/* The effective mode changed since the last call: true with what it was
 * (MG_LINKPOL_NO_MODE for the first report of a connection), what it is and
 * why. */
bool mg_linkpol_take_change(mg_linkpol_t *p, uint8_t *from, mg_linkpol_mode_t *to, const char **why);
/* The parameters a mode asks for. */
mg_linkpol_params_t mg_linkpol_params(const mg_linkpol_t *p, mg_linkpol_mode_t mode);
/* link_params (setting 17): interval, latency, timeout in effect, little-endian. */
void mg_linkpol_link_params(const mg_linkpol_t *p, uint8_t out[6]);
const char *mg_linkpol_mode_name(uint8_t mode);
/* The bench switch (link.wake=0|1; CONFIG_MUSE_GADGET_BLE_LINK_WAKE at boot, off by default): off closes an open window. */
void mg_linkpol_set_wake(mg_linkpol_t *p, bool on, int64_t now_us);
/* What the caller does with its override now; w is its own state (zero at
 * start). After a disconnect it always ends released. */
mg_linkpol_wake_act_t mg_linkpol_wake_sync(const mg_linkpol_t *p, mg_linkpol_wake_t *w);
/* The pending notification's period on controllers without a latency
 * override: half the interval in effect (1.25 ms units), at least
 * MG_LINKPOL_WAKE_PERIOD_MIN_US, so a notification is queued at every event. */
uint32_t mg_linkpol_wake_period_us(uint16_t itvl);
/* The BLE spec's connection parameter ranges (Core Vol 6 Part B 4.5.1 and
 * Vol 3 Part A 4.20): interval 6-3200 (7.5 ms-4 s) with min <= max, latency
 * 0-499, timeout 10-3200 (100 ms-32 s), and timeout > (1 + latency) x
 * itvl_max x 2. */
bool mg_linkpol_sleep_ok(uint16_t itvl_min, uint16_t itvl_max, uint16_t latency, uint16_t timeout);
/* Apple's accessory rules (Accessory Design Guidelines, Connection
 * Parameters): a set outside them may be refused by an iPhone. */
bool mg_linkpol_apple_ok(const mg_linkpol_params_t *s);
/* The bench knob: the sleep mode asks itvl_min-itvl_max / latency /
 * MG_LINKPOL_TIMEOUT from now on (false, nothing changed, if
 * mg_linkpol_sleep_ok refuses it); mg_linkpol_sleep_default() restores the
 * defaults. Either asks the new set at once while the link is in sleep. */
bool mg_linkpol_set_sleep(mg_linkpol_t *p, uint16_t itvl_min, uint16_t itvl_max, uint16_t latency);
void mg_linkpol_sleep_default(mg_linkpol_t *p);
/* The console words: link.sleep=ITVL_MIN,ITVL_MAX,LAT, link.sleep=0 and
 * link.sleep?; anything else starting with link.sleep gets the usage line.
 * False if line isn't a link.sleep word; else the answer is in out. */
bool mg_linkpol_sleep_word(mg_linkpol_t *p, const char *line, int64_t now_us, char *out, size_t n);
static inline bool mg_linkpol_active(const mg_linkpol_t *p)
{
    return p->mode == MG_LINKPOL_ACTIVE;
}
