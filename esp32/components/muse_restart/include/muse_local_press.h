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

#include <stdbool.h>
#include <stdint.h>

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

/*
 * The update's confirm press (protocols/mgcommands.h, Token proof; test-vectors/mg-update-v2.json, whose reference
 * mg_update_ref.py this follows): on a release build a Bluetooth image change (upload, image state write, erase,
 * reset) from a link whose token proof hasn't matched, set up or not (no device token, or after Forget), needs no
 * phone, no token and no pre-armed hold: ONE physical press on the gadget, any key, the talk key included. Each change
 * is asked about whole (mg_ble.c's request front, mg_smp_front.h, ahead of the SMP core).
 *   - Asking: a change starts the prompt, MUSE_UPDATE_PROMPT_MS ("Press to confirm update" on the update card, a
 *     touch board's Confirm; the status light's confirm pattern on a screenless board). Each change while it asks is
 *     answered rc 10 (EBUSY); the client retries about once a second.
 *   - What can't be asked: an upload's first chunk that names no whole-image SHA-256, and a chunk with no upload
 *     started on its link, get rc 11 and no prompt (a press would have no image to bind to).
 *   - The press: the gadget's own key while the prompt shows (muse_input.c, main/button.c), or a touch board's
 *     Confirm (muse_ui.c). A press at any other time never counts (nobody in range can wait for the owner's touch).
 *   - Bound: a press on a prompt an upload started binds to that upload (its length and SHA-256) for
 *     MUSE_UPDATE_BOUND_MS, across link drops. Within it the same upload starts again or resumes on any link with no
 *     new prompt, and the link that carries it goes on: its chunks, the state write that stages it, an erase or the
 *     reset. A transfer under way when the time ends finishes on its link; starting it again, on that link or another,
 *     asks again, and so does any other upload. A write past the bound length, a last chunk after which the slot
 *     doesn't hash to the bound SHA-256, and a state write while it doesn't (the caller hashes: slot_ok) get rc 11 and
 *     end the binding.
 *   - Covered: a press on a prompt an erase, a reset or a state write started covers only that request (its group,
 *     command and body: key) on its link, for MUSE_UPDATE_BOUND_MS; any other change asks again.
 *   - The prompt's timeout, or a cancel on the gadget (a touch elsewhere on the card): the next change is rc 11, once,
 *     and one after that asks again (within the re-arm limit below); a cancel binds nothing. A new link ends the
 *     prompt, its part in the bound upload and what a press covered on it.
 *   - Re-arm: the prompt shows at most once per MUSE_UPDATE_REARM_MS per link; a change that would show it sooner
 *     waits (rc 10, nothing shown) and the client's retries bring it up after. A new link may show it at once.
 *   - Forget (the device token erased) ends the binding, what a press covered and the prompt (the link's proof is
 *     mg_proto's, and a restart follows the erase); the wired console's arm stays.
 *   - A passcode lock asks for the passcode first: the ask wakes the gadget to its keypad ("Unlock to update"), rc 10
 *     while locked, a press confirms nothing, and the confirm prompt shows once unlocked, within the same 30 s from
 *     the wake (muse_local_press_set_ui).
 *   - No press: a matched proof (the owner's app), the wired console's arm (update.arm, the USB or UART console
 *     only: the link that's up, or the next one, for MUSE_LOCAL_PRESS_MS), a recovery the user's button entered, and
 *     bench, automation and test builds (mg_smp_gate.h). A recovery the gadget entered itself has no way to prompt:
 *     rc 11, set up or not.
 * Injected input (key=, !IN, the Nordic UART, the bench) never confirms, in any build. Reads stay open. Known limit:
 * on a release gadget with a token, a nearby device can start a firmware update without the token proof, and the
 * owner's next physical button press (the talk key included) installs it. Where images are signed, they are signed
 * with the repository's development key, which is public, so a valid signature doesn't show who built the image.
 */
#define MUSE_LOCAL_PRESS_MS 60000
/* How long the prompt waits for the press (mg-update-v2.json prompt_timeout_ms). */
#define MUSE_UPDATE_PROMPT_MS 30000
/* How long the press stays bound to its upload, across link drops, or covers its other change on its link
 * (mg-update-v2.json confirm_window_ms). */
#define MUSE_UPDATE_BOUND_MS 600000
/* A new prompt shows at most once per this per link, as the re-setup prompt does (mg-update-v2.json
 * prompt_rearm_ms): a change that would show it sooner is answered rc 10 with nothing shown. */
#define MUSE_UPDATE_REARM_MS 10000

/* The upload an upload's first chunk names: its length and whole-image SHA-256 (or none). */
typedef struct {
    uint32_t len;
    bool has_sha;
    uint8_t sha[32];
} muse_update_ident_t;

typedef enum {
    MUSE_UPDATE_UPLOAD0 = 0,   /* an image upload's first chunk (offset 0, naming its length) */
    MUSE_UPDATE_CHUNK,         /* a later chunk */
    MUSE_UPDATE_STATE,         /* an image state write */
    MUSE_UPDATE_CHANGE,        /* an erase or the reset */
} muse_update_kind_t;

/* One change, as the request front read it (mg_smp_front.h). */
typedef struct muse_update_req {
    uint8_t kind;                /* muse_update_kind_t */
    muse_update_ident_t id;      /* UPLOAD0: the image it names */
    uint32_t off, n;             /* UPLOAD0, CHUNK: where it writes, and how many bytes */
    uint8_t key[32];             /* STATE, CHANGE: SHA-256 of its group, command and body (what a press covers) */
    /* The slot, hashed by the caller against slot_for (the binding when it looked: muse_local_press_bound): for a
     * write that ends at the bound length, whether the slot with it is the bound image (len bytes hashing to its
     * SHA-256); for a state write, whether the slot is now. Ignored if the binding changed meanwhile. */
    bool slot_ok;
    muse_update_ident_t slot_for;
} muse_update_req_t;

typedef enum {
    MUSE_UPDATE_OPEN = 0,   /* confirmed (or armed): the change goes on */
    MUSE_UPDATE_ASKING,     /* the prompt (or the passcode) asks: rc 10 */
    MUSE_UPDATE_REFUSED,    /* rc 11: can't be asked, timed out or cancelled (once), or not the bound image */
} muse_update_answer_t;

#if defined(ESP_PLATFORM) && !CONFIG_MUSE_GADGET_BLE_AUDIO
/* Without BLE gadget audio nothing asks (muse_local_press_ask's only caller is mg_ble.c), so nothing prompts and no
 * press counts. Inline, so the callers' prompt paths (the status light's confirm pattern, the button's and the Muse
 * keys' update press) compile away. */
static inline muse_update_answer_t muse_local_press_ask(const muse_update_req_t *req)
{
    (void)req;
    return MUSE_UPDATE_REFUSED;
}
static inline bool muse_local_press_bound(muse_update_ident_t *id)
{
    (void)id;
    return false;
}
static inline bool muse_local_press_note(const char *what)
{
    (void)what;
    return false;
}
static inline void muse_local_press_cancel(const char *what) { (void)what; }
static inline bool muse_local_press_console(const char *line)
{
    (void)line;
    return false;
}
static inline void muse_local_press_link_new(void) {}
static inline void muse_local_press_forget(void) {}
static inline bool muse_local_press_prompting(void) { return false; }
static inline void muse_local_press_set_ui(bool (*locked)(void), void (*wake)(bool locked))
{
    (void)locked;
    (void)wake;
}
static inline void muse_local_press_set_shown(bool (*shown)(void)) { (void)shown; }
#else
/* An unproven change asks (mg_ble.c's request front). Any task. */
muse_update_answer_t muse_local_press_ask(const muse_update_req_t *req);
/* The upload the press binds to, if any (for slot_ok). Any task. */
bool muse_local_press_bound(muse_update_ident_t *id);
/* A real press on the gadget (what: for the log): confirms the update if the prompt shows (and, with a set_shown
 * surface, is on show) and no passcode lock holds; else nothing (false). */
bool muse_local_press_note(const char *what);
/* The user cancels the prompt on the gadget: the next change is rc 11. */
void muse_local_press_cancel(const char *what);
/* The wired console's update.arm (the USB or UART line readers only, never the Nordic UART or the bench): this link
 * may update with no press. True if line is that word. */
bool muse_local_press_console(const char *line);
/* A new link (BLE connect): the prompt, its part in the bound upload and what a press covered on the last one end.
 * Any task. */
void muse_local_press_link_new(void);
/* Forget (the device token erased): the binding, what a press covered and the prompt end. Any task. */
void muse_local_press_forget(void);
/* The prompt shows and no passcode lock holds: the UI and the light ask for the press. Any task. */
bool muse_local_press_prompting(void);
/* The board's UI, if it has one: locked (muse_passcode_locked): the passcode comes before the press; wake: an update
 * started asking, so the screen wakes (to the keypad with "Unlock to update" while locked: the 30 s start at the
 * wake, and the confirm prompt follows the unlock). Either may be NULL. */
void muse_local_press_set_ui(bool (*locked)(void), void (*wake)(bool locked));
/* The prompt is on show now: the Muse UI's update card (drawn, nothing over it), or the status light's confirm
 * pattern (not while a voice state takes the ring). A press counts only then (a view over the card must not turn
 * its exit key into the confirm). NULL (no light, no Muse UI): every press during the prompt counts, as before.
 * Any task. */
void muse_local_press_set_shown(bool (*shown)(void));
#endif

/* ---- pure (host tests: tests/test_local_press.py, tests/test_mg_smp.py) ---- */
typedef struct {
    int64_t prompt_ms;          /* the prompt's start; -1: none */
    int64_t shown_ms;           /* when this link last showed the prompt (the re-arm limit); -1: never */
    bool cancelled;             /* the user cancelled the prompt: the next change is refused */
    bool ask_upload;            /* the prompt's change was an upload (ask), else a change (ask_key) */
    muse_update_ident_t ask;
    uint8_t ask_key[32];
    int64_t bound_ms;           /* the press on an upload; -1: no binding */
    muse_update_ident_t bound;  /* the upload it binds to */
    bool here;                  /* this link carries the bound upload */
    int64_t covered_ms;         /* the press on another change, on this link; -1: none */
    uint8_t covered_key[32];    /* that change */
    uint32_t ends;              /* bindings ended by a refused write (the caller drops the transfer) */
    int64_t armed_ms;           /* the wired console's arm; -1: none */
    bool arm_link, arm_next;    /* it counts for this link / for the next one too */
} muse_local_press_t;

void muse_local_press_core_init(muse_local_press_t *w);
muse_update_answer_t muse_local_press_core_ask(muse_local_press_t *w, int64_t now_ms, bool locked,
                                               const muse_update_req_t *req);
bool muse_local_press_core_bound(const muse_local_press_t *w, muse_update_ident_t *id);
/* A press: confirms (true) only while the prompt shows and no lock holds. */
bool muse_local_press_core_note(muse_local_press_t *w, int64_t now_ms, bool locked);
/* (A lock doesn't stop the prompt's clock: the keypad's unlock and the press share its MUSE_UPDATE_PROMPT_MS.) */
void muse_local_press_core_cancel(muse_local_press_t *w, int64_t now_ms);
void muse_local_press_core_arm(muse_local_press_t *w, int64_t now_ms);
void muse_local_press_core_link_new(muse_local_press_t *w);
void muse_local_press_core_forget(muse_local_press_t *w, int64_t now_ms);
bool muse_local_press_core_prompting(const muse_local_press_t *w, int64_t now_ms);
