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

#include "mg_config.h"
#include "mg_resample.h"
#include "sbc.h"
#if MG_WITH_LC3
#include "lc3.h"
#endif

/*
 * Audio playback (mg_command_stream_audio): the client's encoded frames go
 * into a byte ring (mg_play_t, filled from BLE writes); a playback task pops
 * whole frames and decodes them (mg_dec_t) to the speaker's 16 kHz mono.
 * No locking here: the caller serializes mg_play_* calls; mg_dec_t belongs
 * to the playback task alone.
 */

#define MG_PLAY_OUT_RATE 16000
#define MG_PLAY_MAX_BITRATE_KBPS 256
#define MG_PLAY_MAX_CHANNELS 1
/*
 * Live reply audio (mgcommands.h "Live reply audio"): the stream
 * starts once it holds MG_PLAY_PREBUFFER_US (at most 40 ms) and never
 * re-buffers: a gap is concealed (mg_dec_conceal: LC3's packet loss
 * concealment, else a faded repeat, then silence) and playout resumes with
 * the next frame that arrives. With the output path's 30 ms of I2S DMA and
 * the client's 60 ms start burst, the whole depth stays under 100 ms.
 */
#define MG_PLAY_PREBUFFER_US 30000         /* start with this much (once: a gap is concealed, not re-buffered) */
/*
 * A reply that starts while the link isn't active (on the
 * Watcher, a chat reply on a ready link, 30-45 ms
 * at latency 5, active granted ~1 s later, played with 6 underruns at the
 * 30 ms start) starts at MG_PLAY_SLOW_START_US held instead, or at its end
 * of data if the whole clip is shorter: stop_streaming keep, or no data for
 * MG_PLAY_SLOW_END_US with less held. The owner sets mg_play_t.slow_link
 * while the link in effect isn't active's set; once active is granted the
 * normal start applies (to a stream still prebuffering too).
 */
#define MG_PLAY_SLOW_START_US 150000
/*
 * Adaptive start: per connection, a reply
 * starts at mg_play_t.start_us held, MG_PLAY_PREBUFFER_US
 * at connect. A reply that underran in its first second of audio
 * (MG_PLAY_EARLY_US; st.early_underruns) raises the next one's start
 * MG_PLAY_START_STEP_UP_US, up to the live depth's floor
 * (mg_play_live_floor_us, the listed 70 ms) at most; each clean reply (no
 * underrun at all) eases it back MG_PLAY_START_STEP_DOWN_US, never below
 * MG_PLAY_PREBUFFER_US. A reply with only later underruns leaves it. Judged
 * when the next stream starts (mg_play_start), on the stream before it if it
 * played audio and wasn't kept open; mg_play_live_forget (a new connection)
 * resets it. A kept-open stream keeps MG_PLAY_PREBUFFER_US and the slow
 * link's MG_PLAY_SLOW_START_US is unchanged. Each move is left in
 * mg_play_t.start_note for the owner to log (`start threshold 50 ms (early
 * underrun last reply)`); early underruns still count as underruns.
 */
#define MG_PLAY_EARLY_US 1000000
#define MG_PLAY_START_STEP_UP_US 20000
#define MG_PLAY_START_STEP_DOWN_US 10000
#define MG_PLAY_SLOW_END_US 100000
#define MG_PLAY_CHUNK_US 10000             /* what the playback task pops, decodes and writes at a time */
#define MG_PLAY_CONCEAL_MAX_US 60000       /* a gap concealed this long, then silence until data */
#define MG_PLAY_DEPTH_UNKNOWN UINT32_MAX   /* mg_play_t.out_us: the output path's depth isn't known */
#define MG_PLAY_QUEUED_UNKNOWN 0xffff      /* buffer_update's queued ms: unknown */
#define MG_PLAY_STARVE_END_US 10000000     /* starved this long: the device ends the stream */
/*
 * A gap this long is the space between replies in one stream, not an
 * underrun: an underrun is counted when audio comes back within it (at the
 * gap's start on the stream's clock).
 */
#define MG_PLAY_IDLE_US 400000
/* "Playing" for the face and the panel hold (mg_play_audible): audio held,
 * or decoded audio played within this much of the stream's clock. */
#define MG_PLAY_AUDIBLE_TAIL_US 300000
/*
 * Live depth (mgcommands.h mg_stream_audio_parameter_live_depth_ms and
 * capability live_depth). The
 * cap is the client's choice: only a stream started with parameter 6 holds a
 * depth (mg_play_params_t.live_depth_ms); without it the credit is the whole
 * ring (an app that doesn't pace live never underruns
 * against a cap it didn't ask for).
 *
 * - The depth is audio held on the device (queued ms: the ring, what the
 *   playback task holds, the output path): max(asked, the floor). The floor,
 *   listed as the capability's value, is the start threshold, the decoder's
 *   pending chunk and the board's output path (mg_play_live_floor_us: 30 +
 *   10 + the I2S DMA's depth, 30 ms on every board with musegadgets
 *   playback, so 70 ms).
 * - Audio in flight doesn't count against it (with ~70 ms in
 *   flight, a cap on queued + in flight left ~20 ms on the device, ~180
 *   underrun blocks a reply). The credit is depth - queued, plus what the
 *   device sees in flight: the audio the link takes from a credit to the data
 *   it allows (mg_proto measures it: the time from a buffer_update that raised
 *   the client's allowed total to the first byte past the total before it,
 *   smoothed, at the stream's rate; at most MG_PLAY_LIVE_FLIGHT_MAX_US), plus
 *   the in-flight allowance for what the measurement misses (the link's
 *   jitter). A credit counted against the client's whole allowed total
 *   instead (its written bytes are invisible to the device) grants each
 *   credit again before the client can use it: on a 15 ms link with 0-30 ms
 *   of jitter the depth swung 40-180 ms.
 * - The allowance (mg_play_t.live_allow_us) starts at 0, grows
 *   MG_PLAY_LIVE_GROW_US per underrun up to MG_PLAY_LIVE_ALLOW_MAX_US, and
 *   shrinks MG_PLAY_LIVE_SHRINK_US per MG_PLAY_LIVE_CLEAN_US of audio played
 *   without one. Like the measurement it is the link's, so it lasts across
 *   streams until the owner forgets it (mg_play_live_forget: a new
 *   connection).
 */
#define MG_PLAY_LIVE_DEPTH_MAX_MS 1000      /* start_streaming refuses 0 or more than this */
#define MG_PLAY_LIVE_FLIGHT_MAX_US 250000   /* the most in flight a credit counts out */
#define MG_PLAY_LIVE_GROW_US 20000          /* the allowance grows this much per underrun ... */
#define MG_PLAY_LIVE_ALLOW_MAX_US 40000     /* ... to at most this */
#define MG_PLAY_LIVE_SHRINK_US 5000         /* and shrinks this much ... */
#define MG_PLAY_LIVE_CLEAN_US 5000000       /* ... per this much audio played without an underrun */
#define MG_PLAY_MAX_POP 1024               /* bytes of frames per mg_play_pop() */
#define MG_PLAY_MAX_OUT 2048               /* samples per mg_dec_run() */

typedef struct {
    uint8_t codec;           /* mg_data_type_t */
    uint32_t rate;           /* Hz */
    uint8_t channels;
    uint16_t bitrate_kbps;   /* 0: not given */
    uint16_t frame_bytes;    /* LC3: bytes per frame; others: 0 */
    uint16_t live_depth_ms;  /* start_streaming's live depth (parameter 6); 0: not given, the whole ring */
} mg_play_params_t;

typedef enum {
    MG_PLAY_IDLE,
    MG_PLAY_PREBUFFER,       /* collecting MG_PLAY_PREBUFFER_US at the start (silence plays meanwhile) */
    MG_PLAY_PLAYING,
    MG_PLAY_DRAINING,        /* stop_streaming keep: playing out what's left */
    MG_PLAY_DONE,            /* over: the playback task notifies stop_streaming (done_keep says which) */
} mg_play_state_t;

typedef struct {
    uint32_t bytes_received;    /* all Data bytes since start_streaming, as buffer_update reports */
    uint32_t bytes_dropped;     /* writes that didn't fit, or weren't whole frames */
    uint32_t writes_dropped;
    uint32_t bad_frames;        /* writes rejected as not whole frames of the stream's format */
    uint32_t frames_in;
    uint32_t frames_popped;
    uint32_t underruns;         /* times it ran dry while playing */
    uint32_t silence_us;        /* silence played for underruns and prebuffering */
    uint32_t clock_us;          /* the stream's time: every chunk the speaker took since start_streaming */
    uint32_t last_underrun_us;  /* clock_us at the latest underrun (MG_PLAY_NO_UNDERRUN: none) */
    uint32_t flushes;           /* mg_stream_audio_flush */
    uint32_t flushed_us;        /* audio they discarded */
    uint32_t gap_us;            /* the underruns' gaps, in all (each concealed, so the reply runs that late) */
    uint32_t idle_gaps;         /* gaps of MG_PLAY_IDLE_US or more: between replies */
    uint32_t live_grown, live_shrunk;   /* the live depth's in-flight allowance: steps up and down */
    uint32_t early_underruns;   /* underruns in the reply's first MG_PLAY_EARLY_US of audio (adaptive start) */
    /* mg_stream_audio_video_anchor (lip sync): anchors resolved, and the
     * latest one's hold (silence played until its picture showed) and where the
     * picture stood when its sample started (shown - anchored, in frames: 0 on
     * time, below 0 the 500 ms cap ran out first, above 0 audio already playing
     * or the picture already past it). */
    uint32_t anchors;
    uint32_t anchors_held;      /* ...of them, held (audio started after a gap) */
    uint32_t anchor_held_us;
    int32_t anchor_offset;
    bool anchor_measured;       /* the latest landed while audio played: measured, not held */
} mg_play_stats_t;

#define MG_PLAY_NO_UNDERRUN UINT32_MAX

typedef struct {
    uint32_t size;
    uint8_t *buf;               /* allocated at start, freed by mg_play_release */
    uint32_t head, len;
    uint32_t buffered_us;       /* audio held */
    /* Popped but not yet written to the speaker (the playback task's
     * decoder and its chunk FIFO): mg_play_pop adds what it takes, the
     * playback task sets what's left after each write (mg_play_set_pending). */
    uint32_t pending_us;
    /* The output path after the speaker write (the I2S DMA's buffers at the
     * output rate), set by the owner; MG_PLAY_DEPTH_UNKNOWN when it can't say. */
    uint32_t out_us;
    mg_play_state_t state;
    mg_play_params_t params;
    uint32_t generation;        /* bumped by every start and stop */
    uint32_t stream;            /* bumped by every start: the playback task starts over on a change */
    bool done_keep;
    bool starving;              /* ran dry while playing: the playback task conceals (mg_play_concealing) */
    uint32_t gap_at;            /* clock_us where this gap began */
    uint32_t starve_us;
    bool slow_link;             /* set by the owner: the link isn't active, a stream starts at MG_PLAY_SLOW_START_US */
    uint32_t write_clock_us;    /* clock_us at the stream's latest write (the slow start's end of data) */
    /* mg_stream_audio_flush: the playback task discards its FIFO and resets
     * its decoder when flush_seq moves; flushed until the next audio (no
     * concealment, not an underrun, queued ms 0). */
    uint32_t flush_seq;
    uint32_t flush_us;          /* what the latest flush discarded */
    bool flushed;
    uint32_t audio_clock_us;    /* clock_us after the latest chunk that carried audio */
    /* What the stream's writes brought (accepted, whole frames): its byte
     * rate for the live depth's credit when the codec doesn't fix one (SBC). */
    uint64_t in_bytes, in_us;
    /* Live depth (above): the depth held (0: none, the whole ring), the
     * in-flight allowance (kept across streams) and the audio played since
     * the allowance last moved or the stream last underran. */
    uint32_t live_us;
    uint32_t live_allow_us;
    uint32_t live_clean_us;
    /* Adaptive start (above): the start threshold for a reply
     * (0: MG_PLAY_PREBUFFER_US), kept across streams until a new connection,
     * and this stream's first audio on its clock. */
    uint32_t start_us;
    uint32_t play_at_us;
    bool start_skip;            /* a new link since the last stream: that stream isn't judged */
    const char *start_note;     /* the last move's reason, for the owner's log ("early underrun last reply"); it clears */
    /* A/V anchor (mg_stream_audio_video_anchor): the owner's reader of
     * the picture showing now (the video frame number extended to 32 bits;
     * false: none showing), set once; NULL: no video. The
     * anchor names the first sample of the next write after it (anchor_at, on
     * the written-bytes count wr_total; rd_total counts what was popped). */
    bool (*shown32)(uint32_t *frame);
    uint32_t wr_total, rd_total;
    bool anchor_on;
    bool anchor_holding;        /* silence plays until the anchored picture shows (not concealment, not an underrun) */
    uint32_t anchor_frame, anchor_at, anchor_wait_us;
    /* A reply's first audio with video showing and no anchor yet: held up to
     * MG_PLAY_ANCHOR_WAIT_US for one (it may arrive just after its audio); an
     * anchor that comes meanwhile names the held sample. */
    bool pre_holding;
    uint32_t pre_wait_us;
    /* The audio clock the reply's picture follows (mg_play_av_pos): the
     * latest resolved anchor's picture and its sample's place in the stream's
     * content (in_us's count), taken at the anchor's Control write
     * (av_*_next) and made current when that sample comes up; av_ok from then
     * until a flush, a dropped anchor or a new stream. */
    uint32_t av_frame_next, av_frame;
    uint64_t av_ref_next_us, av_ref_us;
    bool av_ok;
    mg_play_stats_t st;
} mg_play_t;

/* An anchor holds a reply's first sample at most this long. */
#define MG_PLAY_ANCHOR_MAX_US 500000
/* A reply's first audio waits this long for an anchor that hasn't come:
 * only with video showing (shown32), only at a stream's start or after the gap between replies. */
#define MG_PLAY_ANCHOR_WAIT_US 60000

void mg_play_init(mg_play_t *p, uint32_t buffer_bytes);

/* Capabilities, the default first. */
size_t mg_play_codecs(uint8_t *out, size_t cap);
size_t mg_play_rates(uint32_t *out, size_t cap);

/*
 * Fills the omitted parameters (codec is required) and checks them:
 * 0 if playable, else mg_error_code_invalid_value.
 */
uint16_t mg_play_check(mg_play_params_t *params);

/* Audio is playing (mgcommands.h: "reply playback" means audio playing, not
 * the stream open): the stream holds some, or played decoded audio within
 * MG_PLAY_AUDIBLE_TAIL_US of its clock. False between replies on an open
 * stream, once that's past. The face and the panel hold follow this. */
bool mg_play_audible(const mg_play_t *p);

/*
 * mg_stream_audio_flush: discards everything held for the open stream at once
 * (the ring now, the playback task's FIFO and decoder state when it sees
 * flush_seq move), keeps the stream open with its counters running. Applied
 * where the Control write is handled, in order with the Data writes around it.
 * Returns false with no stream to flush.
 */
bool mg_play_flush(mg_play_t *p);

/*
 * mg_stream_audio_video_anchor (mgcommands.h): the first sample of the
 * next Data written after it belongs with picture `frame`. Applied where the
 * Control write is handled, in order with the Data writes around it. When that
 * sample comes up after a gap (a reply's first: the stream starting, or empty
 * since its last audio) and the picture showing hasn't reached `frame`
 * ((int32_t)(shown - frame) < 0), silence plays until it does, at most
 * MG_PLAY_ANCHOR_MAX_US; an anchor whose sample comes up while audio plays is
 * only measured. Never pauses or skips audio. A newer anchor replaces a
 * pending one. A reply's first audio that came before its anchor (with
 * video showing) waits up to MG_PLAY_ANCHOR_WAIT_US for it, and an anchor in
 * that time names that held sample. False with no stream or no reader of the
 * picture (shown32).
 */
bool mg_play_anchor(mg_play_t *p, uint32_t frame);
/* A flush, or the video stream stopping or restarting: a pending anchor goes
 * and the audio plays at once. */
void mg_play_anchor_drop(mg_play_t *p);
/*
 * The audio clock for the reply's picture: true while a reply's audio
 * plays steadily after a resolved anchor (playing or draining, not starved,
 * not held for an anchor, the output path's depth known, no flush since),
 * with *frame the anchor's picture and *pos_us where the sound being heard
 * now stands from that anchor's sample, in microseconds of the stream's
 * content: what came in since it, less what the ring, the playback task and
 * the output path still hold (buffered_us + pending_us + out_us; may be
 * negative just before the anchored sample is heard). The picture that
 * belongs with it is *frame + pos_us x fps / 1e6. False otherwise: no
 * clock, and the picture is drawn as without one.
 */
bool mg_play_av_pos(const mg_play_t *p, uint32_t *frame, int32_t *pos_us);

/* Starts a stream (restarting any). False if the buffer can't be allocated.
 * A live depth (params->live_depth_ms) holds max(it, the floor). */
bool mg_play_start(mg_play_t *p, const mg_play_params_t *params);
/* Frees the buffer once idle. */
void mg_play_release(mg_play_t *p);
bool mg_play_active(const mg_play_t *p);
uint32_t mg_play_available(const mg_play_t *p);
/*
 * buffer_update's available bytes, the credit (mgcommands.h: the client keeps
 * its bytes in flight past the total received below it). Without a live
 * depth: mg_play_available, the ring's room. With one (live_us): the depth
 * less queued ms (ring + task + output path; the path left out when unknown),
 * plus flight_us (the link's measured share in flight, at most
 * MG_PLAY_LIVE_FLIGHT_MAX_US) and the allowance, at the stream's byte rate
 * (LC3: frame bytes per 10 ms; PCM: 2 bytes a sample; SBC: its bitrate if
 * given, else what its writes brought, else the maximum bitrate), in whole
 * LC3 frames or PCM samples, within the ring's room; 0 when that comes to
 * nothing. Writes already past it are kept, not dropped: they just aren't
 * credited again until the depth falls.
 */
uint32_t mg_play_credit(const mg_play_t *p, uint32_t flight_us);
/* The live depth's floor (mgcommands.h capability live_depth): the start
 * threshold, the decoder's pending chunk and the output path (left out when
 * unknown). */
uint32_t mg_play_live_floor_us(const mg_play_t *p);
/* A new link: the in-flight allowance starts over (mg_proto's measurement
 * too), and the adaptive start goes back to MG_PLAY_PREBUFFER_US. */
void mg_play_live_forget(mg_play_t *p);
/* The start threshold a reply takes now (adaptive start). */
uint32_t mg_play_start_threshold_us(const mg_play_t *p);
uint32_t mg_play_received(const mg_play_t *p);

/* One Data write: whole frames. Returns true if it went into the buffer. */
bool mg_play_write(mg_play_t *p, const uint8_t *data, size_t len);
/* A write refused whole (past the granted credit): counted received and dropped, nothing buffered. */
void mg_play_drop_write(mg_play_t *p, size_t len);
/* stop_streaming: keep plays out the buffer (DRAINING, then DONE); drop empties it (IDLE). */
void mg_play_stop(mg_play_t *p, bool keep);

/*
 * The playback task's side: pops whole frames worth at least `chunk_us`
 * (or what there is) into `out`, and returns their bytes; 0 means play
 * `chunk_us` of silence (prebuffering or an underrun), which it counts.
 */
size_t mg_play_pop(mg_play_t *p, uint8_t *out, size_t cap, uint32_t chunk_us);

/* mg_play_pop returned nothing in the middle of a stream (a gap, not the
 * start's prebuffer): conceal this chunk rather than play silence. */
bool mg_play_concealing(const mg_play_t *p);

/* What the playback task still holds, popped but not yet written. */
void mg_play_set_pending(mg_play_t *p, uint32_t us);

/*
 * buffer_update's queued ms (mgcommands.h, A/V sync): the audio held ahead
 * of the speaker right now, the ring plus the playback task's pending audio
 * plus the output path (out_us), rounded; 0 with no stream;
 * MG_PLAY_QUEUED_UNKNOWN when out_us is MG_PLAY_DEPTH_UNKNOWN.
 */
uint16_t mg_play_queued_ms(const mg_play_t *p);

/* Duration of `len` bytes of whole frames in the stream's format, or -1 if they aren't. */
int32_t mg_play_frames_us(const mg_play_params_t *params, const uint8_t *data, size_t len, uint32_t *frames);

/*
 * The once-a-second playback line, for bench tools,
 * logged on mg.play (the Nordic UART mirror) while a stream plays, and once
 * more at its end (end = true):
 *
 *   av play: t=5200 span=1000 underruns=1 new=0 last=140 buf=180 int=23456 low=21000
 *
 *   t          the stream's time in ms (clock_us: since start_streaming)
 *   span       ms of wall time this line covers
 *   underruns  since the stream's start; new: in this window
 *   last       the stream time (ms) of the latest underrun, -1 for none: an
 *              underrun at t >= 200 is one after the reply's first 200 ms
 *   buf        ms of audio buffered now
 *   queued     buffer_update's queued ms now (-1: unknown)
 *   int / low  internal RAM free now / the lowest since boot
 *
 * The end line adds the drift trim's samples (mg_play_format_trimmed):
 * "dropped=N repeated=N".
 */
int mg_play_format_tick(char *out, size_t cap, bool end, const mg_play_stats_t *st, uint32_t prev_underruns,
                        uint32_t span_ms, uint32_t buffered_us, uint16_t queued_ms, uint32_t internal_free,
                        uint32_t internal_low);

/*
 * The trimmed samples on the stream's end line (av play end ... dropped=N
 * repeated=N repaid=N: the drift trim's, and the gaps repaid):
 * mg_play_format_tick's line, then these. Returns its length.
 */
int mg_play_format_trimmed(char *out, size_t cap, uint32_t dropped, uint32_t repeated, uint32_t repaid);

/* ---- drift trim ----------------------------------------------------------
 *
 * Live reply audio: the gadget's output clock may run off the
 * reply's rate, so the depth held ahead of the speaker creeps up (a slow
 * output) or down (a fast one) over a long reply. The playback task holds it
 * at the steady depth the client's start burst gives: the ring plus what the
 * task holds (mg_play_t's buffered_us + pending_us: queued ms less the
 * board's DMA, which doesn't change), averaged over 100 ms windows and
 * smoothed over about 400 ms (the client's packets make a sawtooth, and
 * the link's jitter moves it).
 *
 * - The target is that steady depth, measured: the mean over the stream's
 *   2 s of playing after its first second (the start burst settling). It is
 *   the client's burst less its packets' sawtooth and the link's jitter (a
 *   60 ms burst in 20 ms packets with up to 15 ms of jitter: about 37 ms),
 *   so no constant fits every client: a 60 ms constant would sit 20 ms over
 *   that, and a fast output would run dry before a "40 ms below" rule fired.
 * - More than 40 ms above the target for 2 s drops one sample in 1000
 *   (0.1 %). Below it the sawtooth's troughs run dry first: with their
 *   depth measured beside the target (the floor: the lowest the depth went
 *   meanwhile), a depth more than a third of the floor (40 ms at most)
 *   below the target, but not starved, for 2 s, repeats one in 1000, so a
 *   fast output's troughs still have headroom when it starts.
 * - 0.1 % corrects an output clock up to 0.1 % off. One further off keeps
 *   drifting, so a running trim looks again every 500 ms: still past its
 *   band and not gaining on the target, its rate doubles, up to one in 250
 *   (0.4 %); a faster rate that has brought it back to the target halves,
 *   and 0.1 % stops within 10 ms of it (half the band below, when that's
 *   less), each at most once per 2 s (a clock between two rates holds
 *   there rather than stopping and drifting off again). A clock up to 0.4 % off holds near the target (0.3 %: within
 *   50 ms), its trim stepping between the rates around its error.
 * - With a live depth (mg_play_t.live_us) the credit holds queued ms at the
 *   depth plus the in-flight allowance, so the steady depth moves with the
 *   allowance: the target follows it (live_allow_us now less at the
 *   measurement, added to the measured target), and never sits above the
 *   depth plus the allowance (live_cap_us, less the output path). A depth
 *   over it by the band drops samples back to it; below it the repeat side
 *   runs as before.
 * - Repaying concealed gaps: with the depth
 *   capped (a live depth), every concealed gap adds its length to the
 *   reply's delay for good (the audio waits on the phone). After an underrun
 *   (a gap under MG_PLAY_IDLE_US: mg_play_stats_t.gap_us) the same machinery
 *   owes its length (mg_trim_owe) and drops one sample in 240 (~0.4 %) while
 *   the stream holds 20 ms or more, until it's made up; the drift trim's own
 *   rate waits meanwhile. A reply boundary (a gap of 400 ms or more) or a
 *   flush clears the debt (mg_trim_forgive). Logged `repay 80 ms` / `repaid`.
 * - Each trim is spread over 32 samples (2 ms): those 33 are resampled to 32
 *   (a drop), or 32 to 33 (a repeat), by linear interpolation, so the
 *   waveform stays continuous: no step, no click, a 3 % pitch change for
 *   2 ms. It works on the decoded 16 kHz PCM before the speaker's gain.
 */
#define MG_PLAY_TRIM_SETTLE_US 1000000    /* the target is measured after this much playing ... */
#define MG_PLAY_TRIM_LEARN_US 2000000     /* ... over this much */
#define MG_PLAY_TRIM_BAND_US 40000        /* a trim starts this far off the target (below: floor / 3 at most) ... */
#define MG_PLAY_TRIM_HOLD_US 2000000      /* ... held this long */
#define MG_PLAY_TRIM_DONE_US 10000        /* and stops this close to it */
#define MG_PLAY_TRIM_WINDOW_US 100000     /* the depth's averaging window */
#define MG_PLAY_TRIM_SMOOTH 4             /* then smoothed: 1/4 of each window's change (about 400 ms) */
#define MG_PLAY_TRIM_STEP_US 500000       /* a trim not gaining ground doubles its rate this often */
#define MG_PLAY_TRIM_EVERY 1000           /* one sample in 1000 (0.1 %) to start */
#define MG_PLAY_TRIM_EVERY_MIN 250        /* up to one in 250 (0.4 %) */
#define MG_PLAY_TRIM_SPAN 32              /* samples one trim is spread over */
#define MG_PLAY_REPAY_EVERY 240           /* repaying a gap: one sample in 240 (~0.4 %) ... */
#define MG_PLAY_REPAY_ROOM_US 20000       /* ... while the stream holds this much */

typedef enum {
    MG_TRIM_NONE,
    MG_TRIM_TARGET,   /* the target was measured (target_us) */
    MG_TRIM_START,    /* a trim started (dir, every) */
    MG_TRIM_FASTER,   /* its rate doubled */
    MG_TRIM_SLOWER,   /* back at the target from a faster rate: halved */
    MG_TRIM_STOP,     /* back at the target at 0.1 %: off */
} mg_trim_event_t;

typedef struct {
    int dir;                    /* 0: none; -1: dropping; +1: repeating */
    uint32_t every;             /* one sample in this many */
    /* The depth's averaging window. */
    bool win_open;
    int64_t win_start;
    uint64_t win_sum;
    uint32_t win_n;
    uint32_t win_min;
    int32_t mean_us;            /* the depth, smoothed */
    int64_t gap_from;           /* not playing since (0: playing) */
    /* The target: measured from the first playing window (play_from). */
    bool playing_seen;
    int64_t play_from;
    uint64_t learn_sum;
    uint32_t learn_n, learn_min;
    int32_t target_us;          /* 0 until measured (mg_trim_target: what the trim holds) */
    /* A live depth (set by the caller before each mg_trim_update): the target
     * never above live_cap_us (0: no cap) and moved by the allowance's change
     * since the measurement (allow_at_target). */
    int32_t live_cap_us;
    int32_t live_allow_us;
    int32_t allow_at_target;
    int32_t floor_us;           /* the lowest depth while the target was measured */
    /* Past the band (in past_dir) since past_since, while no trim runs. */
    int past_dir;
    int64_t past_since;
    /* A running trim's last rate check. */
    int64_t step_at;
    int32_t step_off;
    int64_t rate_at;            /* when it last slowed: it slows or stops again only 2 s later */
    uint32_t phase;             /* samples since the last trim */
    uint32_t dropped, repeated; /* the stream's trimmed samples (the drift's) */
    /* Concealed gaps to repay, in samples, and whether the stream holds
     * enough to (set by the caller before mg_trim_run). */
    uint32_t debt;
    bool repay_room;
    bool repaid_now;            /* the debt reached 0 by repaying (cleared by the caller) */
    uint32_t repaid;            /* samples dropped repaying */
} mg_trim_t;

void mg_trim_reset(mg_trim_t *t);
/*
 * One look at the depth (held_us: mg_play_t's buffered_us + pending_us) at
 * now_us. playing: the stream is playing, not prebuffering, draining or in
 * a gap (a gap neither starts a trim nor counts toward one; a running trim
 * goes on). Returns what changed.
 */
mg_trim_event_t mg_trim_update(mg_trim_t *t, int64_t now_us, uint32_t held_us, bool playing);
/* Trims `n` freshly decoded samples in place: returns the new count (n - 1,
 * n or n + 1 per trim due, never past cap). While a debt is owed and
 * repay_room is set, it drops one in MG_PLAY_REPAY_EVERY instead. */
size_t mg_trim_run(mg_trim_t *t, int16_t *pcm, size_t n, size_t cap);
/* The target the trim holds now: the measured one moved by the allowance,
 * within the live cap; 0 until measured. */
int32_t mg_trim_target(const mg_trim_t *t);
/* An underrun's gap, concealed: owe its length. */
void mg_trim_owe(mg_trim_t *t, uint32_t gap_us);
/* A reply boundary or a flush: nothing owed. */
void mg_trim_forgive(mg_trim_t *t);
/*
 * The mg.play line for an event: "trim target 67 ms", "trim -0.1% (depth
 * 130 ms)" (a start, or a new rate), "trim +0.2% (depth 48 ms)", "trim off
 * (depth 99 ms)". Depths and the target are the smoothed depth plus out_us
 * (the board's DMA, as in queued ms; MG_PLAY_DEPTH_UNKNOWN: left out).
 */
int mg_trim_format(char *out, size_t cap, mg_trim_event_t ev, const mg_trim_t *t, uint32_t out_us);
/* "repay 80 ms" (what's owed now) and "repaid". */
int mg_trim_format_repay(char *out, size_t cap, const mg_trim_t *t, bool done);

/* ---- decoding ---------------------------------------------------------- */

typedef struct {
    mg_play_params_t params;
    sbc_t sbc;
#if MG_WITH_LC3
    lc3_decoder_t lc3;
    LC3_DECODER_MEM_T(10000, 24000) lc3_mem;   /* 16 or 24 kHz */
#endif
    mg_resample_t rs;
    int16_t pcm[MG_RESAMPLE_MAX_IN];
    /* Concealment (mg_dec_conceal): the last 10 ms that reached the output,
     * for SBC and PCM's faded repeat, and how long this gap has been
     * concealed (0 once a frame decodes). */
    int16_t last[MG_PLAY_OUT_RATE / 100];
    uint32_t nlast;
    uint32_t conceal_us;
    uint32_t concealed;         /* chunks concealed, for the stats */
    /* Stats for the stream. */
    uint32_t frames, bad, samples_out;
    int32_t peak;
    uint64_t sum_sq;
} mg_dec_t;

bool mg_dec_open(mg_dec_t *d, const mg_play_params_t *params);
void mg_dec_close(mg_dec_t *d);
/* After a flush: the decoder, resampler and concealment start over (the
 * discarded audio isn't concealed); the stream's stats stay. */
bool mg_dec_flush(mg_dec_t *d);
/* Decodes whole frames to MG_PLAY_OUT_RATE mono; returns samples written (<= cap). */
size_t mg_dec_run(mg_dec_t *d, const uint8_t *data, size_t len, int16_t *out, size_t cap);

/*
 * A gap's chunk (us of output): LC3 runs its packet loss concealment
 * (lc3_decode with no data) for the gap's first MG_PLAY_CONCEAL_MAX_US; SBC
 * and PCM repeat the last 10 ms once, faded out, then silence. Returns the
 * samples written (us at 16 kHz).
 */
size_t mg_dec_conceal(mg_dec_t *d, int16_t *out, size_t cap, uint32_t us);

/*
 * The speaker's rewind (the barge-in echo rule: a talk press holds no reply echo): at
 * a talk press's touch-down the playback task clears the output path
 * (mg_ble_platform_t.speaker_clear) so the ~30 ms its DMA held doesn't play
 * into the pre-roll, and keeps what it held unplayed (at most MG_REWIND_MAX
 * samples, already boosted: written as they are). A tap's resume writes them
 * first, ramped in over MG_REWIND_RAMP (they start mid-waveform after
 * silence), then the reply goes on from the FIFO; a hold's claim
 * (stop_streaming drop, or a flush), a client's stop drop or flush,
 * or a new stream drop them. A stop keep plays them, then the rest.
 */
#define MG_REWIND_MAX (MG_PLAY_OUT_RATE * 40 / 1000)   /* 40 ms: the live boards' 30 ms DMA, and room */
#define MG_REWIND_RAMP (MG_PLAY_OUT_RATE / 200)        /* 5 ms, as a flush's fade-out */
/* pcm's first `ramp` samples (of n) rise linearly from near 0 to full; the rest, and n, stay as they are. */
void mg_play_ramp_in(int16_t *pcm, size_t n, size_t ramp);

/* The level of what reached the speaker's codec: muted samples count as zeros. */
typedef struct {
    uint64_t sum_sq;
    int32_t peak;
    uint32_t samples;
    uint32_t muted;             /* samples written with the speaker off */
} mg_level_t;

void mg_level_add(mg_level_t *l, const int16_t *pcm, size_t n, bool muted);

/*
 * Makeup gain for a quiet speaker, with a peak limiter after it: what
 * reaches the codec never goes over MG_BOOST_CEILING, so a codec that adds
 * a few dB of digital gain at the top of its volume range doesn't clip.
 * The limiter follows the peaks at once and lets go over ~64 ms. At 0 dB
 * the stage passes the audio through untouched.
 */
#define MG_BOOST_CEILING 20675     /* -4 dBFS */
typedef struct {
    int db10;                      /* the gain, tenths of a dB */
    int32_t gain_q12;
    int32_t env;                   /* the boosted signal's peak envelope */
    uint32_t limited;              /* samples the limiter turned down */
} mg_boost_t;

void mg_boost_set(mg_boost_t *b, int db10);
void mg_boost_run(mg_boost_t *b, int16_t *pcm, size_t n);
/* The gain for a speaker level: 0 up to half volume, rising to max_db10 at 100. */
int mg_boost_for_volume(int max_db10, int volume);

/*
 * The per-stream stats the device logs on the Nordic UART mirror, as three
 * lines ("stream end (<how>): ...", "stream bytes: ..." and "stream level:
 * decoded ... speaker ...[, speaker off]"; the mirror takes lines of up to
 * 160 characters). `spk` is what reached the codec (NULL: the decoded audio,
 * unmuted). tools/mg_ble_client.py parses them.
 */
int mg_play_format_stats(char *out, size_t cap, const char *how, const mg_play_params_t *params,
                         const mg_play_stats_t *st, const mg_dec_t *d, const mg_level_t *spk);
