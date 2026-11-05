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
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Realtime voice calls with Muse (CONFIG_MUSE_REALTIME_VOICE), beside the
 * push-to-talk voice notes: a tap starts a call, the next press ends it. The
 * call runs over the same Muse session as the notes (muse_chat_session.cpp);
 * muse_voice_call.cpp runs it on the session task, and these are for the
 * voice task.
 *
 * Audio is 16 kHz mono both ways here; the call resamples to and from the
 * wire's 24 kHz. The voice task keeps the mic shut while Muse is audibly
 * talking (muse_vc_gate_t in muse_voice_call_core.h) unless the board's own
 * echo cancellation is confirmed.
 *
 * A compatible VM can end the call with a session_control stop. A press,
 * the server ending the media stream, or the idle and length limits here
 * also end it.
 *
 * A push-to-talk call (muse_voice_call_start_ptt) is the same call with the
 * mic under a button: only audio captured while it's held goes up, and after
 * a release the call sends the rest of the held audio, then quiet audio for
 * as long as it stays up (the server's endpointing closes the turn). A press
 * stops Muse's reply at once, and it never comes back: playback stays off
 * until Muse answers speech from a later hold (see muse_voice_call_core.h,
 * muse_vc_reply_gate_t). Nothing tells Muse to stop generating; its own
 * barge-in on the new speech does that.
 */

typedef enum {
    MUSE_VOICE_CALL_EV_NONE,
    MUSE_VOICE_CALL_EV_READY,     /* live: the mic goes to Muse */
    MUSE_VOICE_CALL_EV_HEARD,     /* text: what Muse heard */
    MUSE_VOICE_CALL_EV_REPLY,     /* text: the last words of Muse's reply */
    MUSE_VOICE_CALL_EV_HOLD,      /* Muse holds the input (switching it): the mic is ignored */
    MUSE_VOICE_CALL_EV_RESUME,
    MUSE_VOICE_CALL_EV_ENDED,     /* text: why. Nothing more comes for this call */
} muse_voice_call_ev_t;

/* Set up, on Wi-Fi and with the session started: a tap can start a call. */
bool muse_voice_call_ready(void);
/* Starts a call (a new one each time); MUSE_VOICE_CALL_EV_ENDED always follows. */
void muse_voice_call_start(void);
/* Starts a push-to-talk call, held (the press that starts it is the first
 * hold). Held audio is buffered while the call connects, so the first words
 * aren't lost; MUSE_VOICE_CALL_EV_ENDED always follows. */
void muse_voice_call_start_ptt(void);
/*
 * A push-to-talk call's button, on every press and release, from the voice
 * task (the same task that calls muse_voice_call_audio and _read), whether
 * the call is live yet or not. A press empties the reply buffer before it
 * returns, and _read returns nothing (flushed) until the session has stopped
 * the old reply; drop whatever you buffered downstream too. After a release
 * no more audio is taken.
 */
void muse_voice_call_set_talking(bool held);
/* Hangs up the current call. */
void muse_voice_call_end(void);
/*
 * Mic audio for the call, 16 kHz mono, as it's captured. False if it was
 * dropped. In a push-to-talk call it's taken only while held (and not while
 * Muse holds the input), from the press on, before the call is live too:
 * up to a few seconds wait for the call, and past that it's dropped and the
 * status says so (overflow).
 */
bool muse_voice_call_audio(const int16_t *pcm, size_t frames);
/*
 * Muse's speech, 16 kHz mono, without waiting. *flushed: the caller barged
 * in, and what's queued past this point was dropped; the caller should drop
 * whatever it buffered downstream too.
 */
size_t muse_voice_call_read(int16_t *pcm, size_t frames, bool *flushed);
/* Non-blocking; events of earlier calls are dropped. */
muse_voice_call_ev_t muse_voice_call_event(char *text, size_t cap);

/*
 * A push-to-talk call's state for the screen, read without locking (each
 * flag on its own may be a moment old). All false with no push-to-talk call.
 * A screen might show: connecting -> "connecting"; talking -> "listening";
 * released with no reply_audio yet -> "thinking"; reply_audio while the
 * speaker still has audio -> "speaking"; released, reply_done and the
 * speaker drained -> ready for the next hold. reply_done means Muse finished
 * generating, not that all of the reply's audio has arrived; more can follow.
 */
typedef struct {
    bool connecting;   /* starting, not live yet (held audio is buffered) */
    bool live;
    bool talking;      /* held, as last set */
    bool capture;      /* muse_voice_call_audio takes audio right now */
    bool sending;      /* held audio, or the quiet tail after it, still going up */
    bool hold;         /* Muse holds the input (switching it) */
    bool heard;        /* Muse's VAD heard speech start since the last press. With
                        * sending false and heard false, Muse most likely heard
                        * nothing (a VAD can lag; a later event still counts) */
    bool reply_open;   /* Muse is answering speech from the last hold: its audio plays */
    bool reply_audio;  /* audible reply audio has been queued since the last press */
    bool reply_done;   /* Muse finished that reply (response.done or turn_settled) */
    bool overflow;     /* audio was dropped during the last hold: the start buffer was full */
    bool reply_lost;   /* a reply was dropped: too much of it came before Muse confirmed the turn */
} muse_voice_call_ptt_status_t;

void muse_voice_call_ptt_status(muse_voice_call_ptt_status_t *out);

/* Provided by the app: the Link node id ("homelink-xxxxxx") the call names as
 * its device, or "" if unknown. Called on the session task, whose stack is in
 * PSRAM: it must not read flash. */
const char *muse_voice_call_device_id(void);

#ifdef __cplusplus
}
#endif
