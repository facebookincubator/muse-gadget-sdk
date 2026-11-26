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
 * A Live call's video beside reply audio on one Data characteristic, replayed
 * on the host through the gadget's own code (tests/test_video_route.py):
 * components/muse_gadget_ble/mg_proto.c (routing, change_data_type, the
 * streams' start and end), components/muse_vstream (the receive ring and its
 * frame format), muse_vgate.h (the decode task's keyframe gate) and
 * muse_vdec.c on the real tinyh264, decoding a real H.264 clip
 * (tests/vectors/h264/testsrc2_208.264). Every picture is checked against the
 * clean clip's own decode, byte for byte.
 *
 * The phone is modeled on a Muse app log with the Watcher: reply
 * audio (16 kHz PCM here) and video interleaved with change_data_type about
 * 25 times a second, video split anywhere, every keyframe its SPS and PPS
 * first, keyframes on request. At the talk press the gadget ends the reply
 * (stop_streaming drop) while the app is mid-write: its audio writes already
 * in flight still arrive, then, once it has seen the stop, change_data_type
 * video and video alone, until the next reply's start_streaming, and
 * interleaving again.
 *
 * Usage: harness CLIP SCENARIO [-v]
 *   barge       the replay above
 *   damage      the same, but the in-flight audio lands in the video ring (as
 *               the old routing did, or any damaged frame)
 *   sps         SPS and PPS only at the stream's start; a damaged parameter
 *               set of the stream's size mid-stream, and one of another size
 *   client_end  the app ends the reply itself (stop_streaming drop) mid-write
 *               and writes video without change_data_type
 *   skip        a P frame lost upstream (numbers 10, 11, 13): its bytes never
 *               come, the ones around it are intact
 *   lost        a P frame lost before the app numbered its frames (numbers
 *               continuous): only its slice's frame_num shows the gap
 *   au          access units with no picture: an SEI alone (an error), an SPS
 *               and PPS alone (not one), between frames of the clip
 *   behind      P frames from number 40 arrive too late to catch up: the gate
 *               drops to the next keyframe and asks for one
 * Prints one JSON line per frame with -v, then a JSON summary.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_play.h"
#include "mg_proto.h"
#include "mgcommands.h"
#include "muse_vdec.h"
#include "muse_vgate.h"
#include "muse_vstream.h"

#define W 208
#define H 208
#define PIC_BYTES (W * H * 3 / 2)
#define MAX_AU 64
#define TICK_MS 40               /* one round of video, then audio: 25 switches to each a second */
#define VIDEO_WRITE 244          /* MTU 247 - 3 */
#define VIDEO_WRITES 6           /* per round, at most */
#define AUDIO_WRITE 320          /* 10 ms of 16 kHz PCM */
#define AUDIO_WRITES 4           /* per round: 40 ms */
#define FRAME_MS 66              /* 15 fps */
#define APP_LATENCY_MS 12        /* stop_streaming to the app's change_data_type (29.763 -> 29.775) */

static int verbose;

/* ---- the clip and its reference pictures --------------------------------- */

static uint8_t clip[256 * 1024];
static uint32_t au_off[MAX_AU], au_len[MAX_AU];
static int nau;
static uint8_t ref[MAX_AU][PIC_BYTES];

static int nal_type_at(const uint8_t *d, uint32_t i) { return d[i + 3] & 0x1f; }

static bool start_code(const uint8_t *d, uint32_t n, uint32_t i)
{
    return i + 3 < n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1;
}

/* Access units: [SPS PPS] and one slice each (the clip is one slice a picture). */
static void split_clip(uint32_t n)
{
    uint32_t start = 0;
    bool slice = false;
    for (uint32_t i = 0; i + 3 < n; i++) {
        if (!start_code(clip, n, i)) {
            continue;
        }
        uint32_t s = i > 0 && clip[i - 1] == 0 ? i - 1 : i;
        int t = nal_type_at(clip, i);
        if ((t == 7 || t == 1 || t == 5) && slice) {
            au_off[nau] = start;
            au_len[nau++] = s - start;
            start = s;
            slice = false;
        }
        if (t == 1 || t == 5) {
            slice = true;
        }
        i += 2;
    }
    au_off[nau] = start;
    au_len[nau++] = n - start;
}

static bool au_idr(int k)
{
    const uint8_t *d = clip + au_off[k];
    for (uint32_t i = 0; i + 3 < au_len[k]; i++) {
        if (start_code(d, au_len[k], i) && (nal_type_at(d, i) == 5 || nal_type_at(d, i) == 1)) {
            return nal_type_at(d, i) == 5;
        }
    }
    return false;
}

/* The access unit without its SPS and PPS (what an app that sends them only at
 * the start would send for a later keyframe). */
static uint32_t au_without_ps(int k, uint8_t *out)
{
    const uint8_t *d = clip + au_off[k];
    uint32_t n = au_len[k];
    for (uint32_t i = 0; i + 3 < n; i++) {
        if (start_code(d, n, i) && (nal_type_at(d, i) == 5 || nal_type_at(d, i) == 1)) {
            uint32_t s = i > 0 && d[i - 1] == 0 ? i - 1 : i;
            memcpy(out, d + s, n - s);
            return n - s;
        }
    }
    memcpy(out, d, n);
    return n;
}

static void decode_reference(void)
{
    muse_vdec_t *v = muse_vdec_open(MUSE_VIDEO_CODEC_H264, W, H);
    static uint8_t buf[64 * 1024];
    for (int k = 0; k < nau; k++) {
        memcpy(buf, clip + au_off[k], au_len[k]);   /* the decoder edits its input in place */
        muse_pic_t p;
        if (muse_vdec_decode(v, buf, au_len[k], &p) != MUSE_VDEC_PICTURE || p.w != W || p.h != H) {
            fprintf(stderr, "reference: frame %d didn't decode\n", k);
            exit(2);
        }
        memcpy(ref[k], p.y, PIC_BYTES);   /* packed planes (muse_vdec.c h264_pic) */
    }
    muse_vdec_close(v);
}

/* ---- a crafted SPS (the 'sps' scenario's damage) --------------------------- */

typedef struct {
    uint8_t b[64];
    int bits;
} bitw_t;

static void put_bit(bitw_t *w, int v)
{
    if (v) w->b[w->bits / 8] |= (uint8_t)(0x80 >> (w->bits % 8));
    w->bits++;
}
static void put_bits(bitw_t *w, uint32_t v, int n) { while (n--) put_bit(w, (int)(v >> n) & 1); }
static void put_ue(bitw_t *w, uint32_t v)
{
    uint32_t x = v + 1;
    int len = 0;
    while ((x >> len) > 1) len++;
    put_bits(w, 0, len);
    put_bits(w, x, len + 1);
}

/* SPS id 0, baseline, w x h macroblocks, a frame_num width and POC type the
 * clip doesn't use: slices parsed against it go wrong. Then PPS id 0 on it. */
static uint32_t crafted_ps(uint8_t *out, uint32_t wmb, uint32_t hmb)
{
    uint32_t n = 0;
    bitw_t w = { { 0 }, 0 };
    put_bits(&w, 66, 8);   /* profile_idc */
    put_bits(&w, 0xc0, 8); /* constraint flags */
    put_bits(&w, 30, 8);   /* level */
    put_ue(&w, 0);         /* sps id */
    put_ue(&w, 12);        /* log2_max_frame_num - 4: 16 bits */
    put_ue(&w, 2);         /* poc type 2 */
    put_ue(&w, 1);         /* num_ref_frames */
    put_bit(&w, 0);        /* gaps */
    put_ue(&w, wmb - 1);
    put_ue(&w, hmb - 1);
    put_bit(&w, 1);        /* frame_mbs_only */
    put_bit(&w, 1);        /* direct_8x8 */
    put_bit(&w, 0);        /* cropping */
    put_bit(&w, 0);        /* vui */
    put_bit(&w, 1);        /* rbsp stop */
    uint32_t bytes = (uint32_t)(w.bits + 7) / 8;
    static const uint8_t sc_sps[] = { 0, 0, 0, 1, 0x67 };
    memcpy(out + n, sc_sps, 5);
    n += 5;
    memcpy(out + n, w.b, bytes);   /* no emulation prevention needed: no 00 00 in it */
    n += bytes;
    bitw_t p = { { 0 }, 0 };
    put_ue(&p, 0);         /* pps id */
    put_ue(&p, 0);         /* sps id */
    put_bit(&p, 0);        /* cabac */
    put_bit(&p, 0);        /* pic order present */
    put_ue(&p, 0);         /* slice groups - 1 */
    put_ue(&p, 0);         /* ref idx l0 - 1 */
    put_ue(&p, 0);         /* ref idx l1 - 1 */
    put_bit(&p, 0);        /* weighted pred */
    put_bits(&p, 0, 2);    /* weighted bipred */
    put_ue(&p, 10);        /* pic_init_qp - 26, as se: +5 -> 10 */
    put_ue(&p, 0);         /* pic_init_qs */
    put_ue(&p, 0);         /* chroma qp offset */
    put_bit(&p, 1);        /* deblocking control present */
    put_bit(&p, 0);        /* constrained intra */
    put_bit(&p, 0);        /* redundant pic cnt */
    put_bit(&p, 1);        /* stop */
    static const uint8_t sc_pps[] = { 0, 0, 0, 1, 0x68 };
    memcpy(out + n, sc_pps, 5);
    n += 5;
    bytes = (uint32_t)(p.bits + 7) / 8;
    memcpy(out + n, p.b, bytes);
    n += bytes;
    return n;
}

/* ---- the gadget: mg_proto with its play buffer and video ring -------------- */

static mg_proto_t gp;
static mg_play_t gplay;
static muse_vstream_t gvs;
static uint8_t gring[128 * 1024];
static uint32_t now_ms;

/* What the app hears (notifications), each delivered APP_LATENCY_MS later. */
typedef struct {
    uint32_t at;
    uint8_t d[32];
    size_t n;
} note_t;
static note_t notes[4096];
static int nnotes, notes_read;

static bool f_notify(void *ctx, mg_ch_t ch, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (ch == MG_CH_CONTROL && nnotes < (int)(sizeof(notes) / sizeof(notes[0])) && n <= sizeof(notes[0].d)) {
        notes[nnotes].at = now_ms + APP_LATENCY_MS;
        memcpy(notes[nnotes].d, d, n);
        notes[nnotes].n = n;
        nnotes++;
    }
    return true;
}
static uint32_t f_now(void *ctx) { (void)ctx; return now_ms; }
static void f_kick(void *ctx) { (void)ctx; }
static bool f_video_begin(void *ctx, const mg_video_params_t *vp) { (void)ctx; (void)vp; return true; }
static void f_video_end(void *ctx) { (void)ctx; }

/* ---- the gadget's decode task (muse_video.c's decode_task, single-threaded) */

static muse_vdec_t *dec;
static muse_vgate_t gate;
static uint8_t au_buf[64 * 1024];   /* s_au: one buffer for every frame, as on the board */
static uint16_t frame_src[65536];   /* the app's frame number -> clip index */

static struct {
    int rx, shown, matched, mismatched, dec_errors, dropped, asks, fast_keys;
    int trouble_at, recovered_at, keys_to_recover, keys_since_trouble, bad_after_recovery;
    int shown_reply2, matched_reply2;
} st = { .trouble_at = -1, .recovered_at = -1 };
/* 'sps': parameter sets that aren't the stream's, ahead of a frame's slice:
 * of its size, in the first keyframe from inject_key_from (where a change is
 * taken: it breaks the keyframe); of its size, in P frame inject_p_same; of
 * another size, in P frame inject_p_other (both refused). */
static int inject_key_from = -1, inject_key_frame = -1, inject_p_same = -1, inject_p_other = -1;
static int inject_result[3] = { -2, -2, -2 };   /* each frame's outcome: 1 its own picture, 0 another, -1 none */
static int reply2_at = -1;
static int skip_number = -1;   /* 'skip': the app's frame that never arrives */
static bool skip_renumber;     /* 'lost': and the next takes its number */
static int skip_result = -2;   /* the frame after it: 1 its own picture, 0 another, -1 not decoded */
static bool skip_asked;        /* request_keyframe went at that frame */
static bool ask_pending;
static int behind_from = -1;   /* 'behind': the first P frame from this number arrives too late to catch up */
static int behind_at = -1;     /* the frame the gate dropped to a keyframe at */
static bool behind_asked;      /* request_keyframe went there */

static void decode_pump(void)
{
    for (;;) {
        muse_vframe_t fr;
        int32_t n = muse_vstream_pop(&gvs, au_buf, sizeof(au_buf), &fr);
        if (n == 0) {
            return;
        }
        st.rx++;
        bool key = n > 0 && (fr.flags & MUSE_VIDEO_FRAME_KEY);
        bool ask = false;
        const char *what;
        int rc = 2;
        bool match = false;
        int src = n > 0 ? frame_src[fr.number] : -1;
        bool after_trouble = st.trouble_at >= 0;   /* keyframes since: not the one that failed first */
        static int last_number = -1;
        bool skipped = n > 0 && last_number >= 0 && (uint16_t)(fr.number - last_number) != 1;
        if (n > 0) last_number = fr.number;
        bool after_skip = n > 0 && skip_number >= 0 &&
                          fr.number == (uint16_t)(skip_number + (skip_renumber ? 0 : 1));
        /* muse_video.c: past LAG_RESYNC_MS of backlog, a P frame drops the decoder to the next keyframe. */
        uint32_t lag_ms = n > 0 && behind_from >= 0 && behind_at < 0 && fr.number >= behind_from ? 800 : 0;
        if (n > 0 && muse_vgate_behind(&gate, key, fr.number, lag_ms, 500, (int64_t)now_ms * 1000, &ask)) {
            behind_at = (int)fr.number;
            behind_asked = ask;
            st.dropped++;
            what = "behind";
            if (st.trouble_at < 0) {
                st.trouble_at = (int)now_ms;
            }
        } else if (!muse_vgate_take(&gate, n, key, fr.after_gap, n > 0 ? fr.number : 0, (int64_t)now_ms * 1000, &ask)) {
            st.dropped++;
            what = n < 0 ? "ring" : fr.after_gap ? "gap" : skipped ? "skipped" : "waiting";
            if (after_skip) {
                skip_result = -1;
                skip_asked = ask;
            }
            if (st.trouble_at < 0 && st.shown) {
                st.trouble_at = (int)now_ms;
            }
        } else {
            muse_pic_t pic;
            rc = muse_vdec_decode(dec, au_buf, (uint32_t)n, &pic);
            bool got = rc == MUSE_VDEC_PICTURE;
            if (got) {
                match = pic.w == W && pic.h == H && !memcmp(pic.y, ref[src], PIC_BYTES);
            }
            bool err = muse_vgate_result(&gate, rc != MUSE_VDEC_ERROR, got, key, (int64_t)now_ms * 1000, &ask);
            if (err) {
                st.dec_errors++;
                st.dropped++;
                what = "error";
                if (st.trouble_at < 0) {
                    st.trouble_at = (int)now_ms;
                }
            } else if (got) {
                st.shown++;
                what = match ? "shown" : "WRONG";
                if (match) {
                    st.matched++;
                } else {
                    st.mismatched++;
                    if (key) st.fast_keys++;
                }
                if (reply2_at >= 0 && (int)now_ms >= reply2_at) {
                    st.shown_reply2++;
                    st.matched_reply2 += match;
                }
            } else {
                what = "no picture";
            }
            if (after_trouble && key) {
                st.keys_since_trouble++;
            }
            if (got && match && st.trouble_at >= 0 && st.recovered_at < 0) {
                st.recovered_at = (int)now_ms;
                st.keys_to_recover = st.keys_since_trouble;
            }
            int which = n <= 0 ? -1 : fr.number == inject_key_frame ? 0 : fr.number == inject_p_same ? 1
                                       : fr.number == inject_p_other ? 2 : -1;
            if (which >= 0) {
                inject_result[which] = got && match ? 1 : got ? 0 : -1;
            }
            if (after_skip) {
                skip_result = got && match ? 1 : got ? 0 : -1;
                skip_asked = ask;
            }
            if (got && !match && st.recovered_at >= 0) {
                st.bad_after_recovery++;
            }
        }
        if (ask) {
            st.asks++;
            ask_pending = true;
            mg_proto_video_request_keyframe(&gp);
        }
        mg_proto_video_stats(&gp, (uint16_t)st.shown, (uint16_t)st.dropped, 1);
        if (verbose) {
            printf("{\"t\": %u, \"number\": %u, \"flags\": %u, \"len\": %d, \"after_gap\": %s, \"src\": %d, "
                   "\"rc\": %d, \"what\": \"%s\", \"asked\": %s, \"resyncs\": %u}\n",
                   now_ms, n > 0 ? fr.number : 0, n > 0 ? fr.flags : 0, (int)n, fr.after_gap ? "true" : "false",
                   src, rc, what, ask ? "true" : "false", gvs.resyncs);
        }
    }
}

/* ---- the phone ------------------------------------------------------------- */

typedef struct {
    /* video */
    uint8_t frame[64 * 1024];
    uint32_t frame_len, frame_sent;
    int next_src;
    uint16_t number;
    uint32_t next_frame_at;
    bool key_asked;
    uint32_t v_sent, v_avail, v_recv;
    /* audio */
    bool audio_on;          /* the app's reply stream is open (as far as it knows) */
    bool audio_cut;         /* it has heard the device's stop: no more audio */
    uint32_t a_sent, a_avail, a_recv;
    uint8_t selected;       /* the type its last change_data_type named */
} app_t;
static app_t app;

static bool ps_every_key = true;     /* the app's SPS and PPS before every keyframe */

static void ctl(const uint8_t *d, size_t n) { mg_proto_write(&gp, MG_CH_CONTROL, d, n); }
static void select_type(uint8_t t)
{
    uint8_t m[2] = { mg_command_change_data_type, t };
    ctl(m, 2);
    app.selected = t;
}
static void data(const uint8_t *d, size_t n) { mg_proto_write(&gp, MG_CH_DATA, d, n); }

static uint32_t rd32(const uint8_t *d) { return d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24; }

static void app_read_notes(void)
{
    while (notes_read < nnotes && notes[notes_read].at <= now_ms) {
        const uint8_t *d = notes[notes_read].d;
        size_t n = notes[notes_read].n;
        notes_read++;
        if (n >= 2 && d[0] == mg_command_stream_video && d[1] == mg_stream_video_request_keyframe) {
            app.key_asked = true;
        } else if (n >= 10 && d[0] == mg_command_stream_video && d[1] == mg_stream_video_buffer_update) {
            app.v_avail = rd32(d + 2);
            app.v_recv = rd32(d + 6);
        } else if (n >= 10 && d[0] == mg_command_stream_audio && d[1] == mg_stream_audio_buffer_update) {
            app.a_avail = rd32(d + 2);
            app.a_recv = rd32(d + 6);
        } else if (n >= 3 && d[0] == mg_command_stream_audio && d[1] == mg_stream_audio_stop_streaming) {
            if (app.audio_on) {
                app.audio_on = false;
                app.audio_cut = true;   /* the gadget ended the reply */
            }
        }
    }
}

static void next_frame(void)
{
    int src = app.next_src;
    if (app.key_asked && !au_idr(src)) {
        while (!au_idr(src)) src = (src + 1) % nau;   /* the encoder's next frame is a keyframe */
    }
    app.key_asked = false;
    bool key = au_idr(src);
    uint8_t *f = app.frame;
    uint32_t len;
    if (key && !ps_every_key && app.number != 0) {   /* SPS and PPS in the stream's first frame only */
        len = au_without_ps(src, f + MUSE_VIDEO_FRAME_HEADER_LEN);
    } else {
        memcpy(f + MUSE_VIDEO_FRAME_HEADER_LEN, clip + au_off[src], au_len[src]);
        len = au_len[src];
    }
    if (key && inject_key_from >= 0 && inject_key_frame < 0 && app.number >= inject_key_from) {
        inject_key_frame = app.number;
    }
    bool other = !key && app.number == inject_p_other;
    if (app.number == inject_key_frame || (!key && app.number == inject_p_same) || other) {
        /* Damaged bytes that look like parameter sets ahead of the slice. */
        uint8_t ps[64];
        uint32_t pn = other ? crafted_ps(ps, 10, 10) : crafted_ps(ps, W / 16, H / 16);
        memmove(f + MUSE_VIDEO_FRAME_HEADER_LEN + pn, f + MUSE_VIDEO_FRAME_HEADER_LEN, len);
        memcpy(f + MUSE_VIDEO_FRAME_HEADER_LEN, ps, pn);
        len += pn;
    }
    f[0] = MUSE_VIDEO_FRAME_MAGIC;
    f[1] = key ? MUSE_VIDEO_FRAME_KEY : 0;
    f[2] = (uint8_t)app.number;
    f[3] = (uint8_t)(app.number >> 8);
    f[4] = (uint8_t)len;
    f[5] = (uint8_t)(len >> 8);
    f[6] = (uint8_t)(len >> 16);
    f[7] = (uint8_t)(len >> 24);
    frame_src[app.number] = (uint16_t)src;
    app.frame_len = MUSE_VIDEO_FRAME_HEADER_LEN + len;
    app.frame_sent = 0;
    app.number++;
    app.next_src = (src + 1) % nau;
}

/* Up to VIDEO_WRITES writes of video, under credits; a frame may stop anywhere. */
static void app_video_round(void)
{
    for (int k = 0; k < VIDEO_WRITES; k++) {
        if (app.frame_sent == app.frame_len) {
            if (now_ms < app.next_frame_at) return;
            next_frame();
            app.next_frame_at += FRAME_MS;
            if (skip_number >= 0 && app.number - 1 == skip_number) {
                if (skip_renumber) {
                    app.number--;   /* lost before it was numbered */
                }
                next_frame();   /* that one was lost on the way: the next goes in its place */
            }
        }
        uint32_t chunk = app.frame_len - app.frame_sent < VIDEO_WRITE ? app.frame_len - app.frame_sent : VIDEO_WRITE;
        /* in flight: written minus received, signed (a gadget that counted
         * stray bytes as video reports more received than written) */
        int64_t in_flight = (int64_t)app.v_sent - (int64_t)app.v_recv;
        if (in_flight + chunk > (int64_t)app.v_avail) return;
        data(app.frame + app.frame_sent, chunk);
        app.frame_sent += chunk;
        app.v_sent += chunk;
    }
}

static uint8_t pcm[AUDIO_WRITE];

static bool app_audio_write(void)
{
    if ((int64_t)app.a_sent - (int64_t)app.a_recv + AUDIO_WRITE > (int64_t)app.a_avail) return false;
    data(pcm, AUDIO_WRITE);
    app.a_sent += AUDIO_WRITE;
    return true;
}

/* The start's own buffer_update reaches the app before it writes (it waits for it). */
static void app_read_notes_now(void)
{
    for (int i = notes_read; i < nnotes; i++) notes[i].at = now_ms;
    app_read_notes();
}

static void start_reply(void)
{
    /* stream_audio start_streaming: PCM, 16 kHz */
    uint8_t m[] = { mg_command_stream_audio, mg_stream_audio_start_streaming, 2, mg_stream_audio_parameter_codec,
                    mg_data_type_audio_pcm, mg_stream_audio_parameter_sample_rate, 0x80, 0x3e, 0, 0 };
    ctl(m, sizeof(m));
    app.audio_on = true;
    app.audio_cut = false;
    app.a_sent = app.a_recv = 0;
    app.a_avail = 0;
    app_read_notes_now();
}

/* ---- 'au': access units with no picture ------------------------------------ */

static int au_case(muse_vdec_t *v, const uint8_t *au, uint32_t n, int want_ref)
{
    static uint8_t buf[64 * 1024];
    memcpy(buf, au, n);
    muse_pic_t p;
    int rc = muse_vdec_decode(v, buf, n, &p);
    if (want_ref >= 0 && rc == MUSE_VDEC_PICTURE) {
        return memcmp(p.y, ref[want_ref], PIC_BYTES) ? 9 : rc;   /* 9: a picture, but not the clip's */
    }
    return rc;
}

static int au_cases(void)
{
    muse_vdec_t *v = muse_vdec_open(MUSE_VIDEO_CODEC_H264, W, H);
    /* SEI, user data unregistered: a UUID and one byte */
    static const uint8_t sei[] = { 0, 0, 0, 1, 0x06, 0x05, 0x11, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                   16, 0x42, 0x80 };
    /* the clip's own SPS and PPS: frame 0 up to its IDR slice */
    uint32_t ps_len = 0;
    const uint8_t *f0 = clip + au_off[0];
    for (uint32_t i = 0; i + 3 < au_len[0]; i++) {
        if (start_code(f0, au_len[0], i) && nal_type_at(f0, i) == 5) {
            ps_len = i > 0 && f0[i - 1] == 0 ? i - 1 : i;
            break;
        }
    }
    int key = au_case(v, clip + au_off[0], au_len[0], 0);
    int sei_rc = au_case(v, sei, sizeof(sei), -1);
    int p1 = au_case(v, clip + au_off[1], au_len[1], 1);
    int ps_rc = au_case(v, f0, ps_len, -1);
    int p2 = au_case(v, clip + au_off[2], au_len[2], 2);
    static const uint8_t empty[] = { 0, 0, 0, 1, 0x09, 0xf0 };   /* an access unit delimiter alone */
    int aud_rc = au_case(v, empty, sizeof(empty), -1);
    int p3 = au_case(v, clip + au_off[3], au_len[3], 3);
    printf("{\"scenario\": \"au\", \"keyframe\": %d, \"sei_only\": %d, \"p1\": %d, \"ps_only\": %d, "
           "\"p2\": %d, \"aud_only\": %d, \"p3\": %d}\n", key, sei_rc, p1, ps_rc, p2, aud_rc, p3);
    muse_vdec_close(v);
    return 0;
}

/* ---- the run --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s CLIP SCENARIO [-v]\n", argv[0]);
        return 2;
    }
    const char *scenario = argv[2];
    verbose = argc > 3 && !strcmp(argv[3], "-v");
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    uint32_t n = (uint32_t)fread(clip, 1, sizeof(clip), f);
    fclose(f);
    split_clip(n);
    decode_reference();

    bool damage = !strcmp(scenario, "damage");
    bool client_end = !strcmp(scenario, "client_end");
    bool skip = !strcmp(scenario, "skip"), lost = !strcmp(scenario, "lost");
    if (skip || lost) {
        skip_number = 12;
        skip_renumber = lost;
    }
    if (!strcmp(scenario, "au")) {
        return au_cases();
    }
    bool behind = !strcmp(scenario, "behind");
    if (behind) {
        behind_from = 40;
    }
    if (!strcmp(scenario, "sps")) {
        ps_every_key = false;
        inject_key_from = 20;
        inject_p_same = 45;
        inject_p_other = 60;
    } else if (strcmp(scenario, "barge") && !damage && !client_end && !skip && !lost && !behind) {
        fprintf(stderr, "unknown scenario %s\n", scenario);
        return 2;
    }

    static const mg_video_caps_t caps = {
        .display_w = 412, .display_h = 412, .round = true, .max_w = 240, .max_h = 240, .max_fps = 30,
        .sug_w = 208, .sug_h = 208, .sug_fps = 15, .sug_kbps = 300,
    };
    muse_vstream_init(&gvs, gring, sizeof(gring), sizeof(gring) / 2);
    mg_play_init(&gplay, 16384);
    mg_proto_config_t cfg = { .play = &gplay, .video = &caps, .vstream = &gvs };
    mg_proto_ops_t ops = { .notify = f_notify, .now_ms = f_now, .play_kick = f_kick, .video_begin = f_video_begin,
                           .video_end = f_video_end };
    mg_proto_init(&gp, &cfg, &ops, NULL);
    mg_proto_connect(&gp);
    mg_proto_set_mtu(&gp, 247);
    mg_proto_subscribe(&gp, MG_CH_CONTROL, true);
    mg_proto_subscribe(&gp, MG_CH_DATA, true);
    for (int i = 0; i < AUDIO_WRITE; i += 2) {
        int16_t s = (int16_t)((i * 37) % 2000 - 1000);
        pcm[i] = (uint8_t)s;
        pcm[i + 1] = (uint8_t)(s >> 8);
    }

    dec = muse_vdec_open(MUSE_VIDEO_CODEC_H264, W, H);
    muse_vgate_start(&gate);

    /* 1. Video (208 x 208, H.264), then the reply's audio beside it. */
    uint8_t sv[] = { mg_command_stream_video, mg_stream_video_start_streaming, 3, mg_stream_video_parameter_codec,
                     mg_data_type_video_h264, mg_stream_video_parameter_width, W, 0, mg_stream_video_parameter_height,
                     H, 0 };
    ctl(sv, sizeof(sv));
    app_read_notes_now();
    app.next_frame_at = 0;
    start_reply();

    const uint32_t press_after = 2000, reply2 = 6000, end = 10000;
    bool pressed = false, released = false, reply2_started = false;
    uint32_t in_flight = 0, discarded_before = 0;
    int press_t = -1, select_video_t = -1;
    static uint8_t pop[4096];

    for (now_ms = 0; now_ms < end; now_ms += TICK_MS) {
        app_read_notes();
        /* the playback task takes 40 ms of audio each round */
        mg_play_pop(&gplay, pop, sizeof(pop), TICK_MS * 1000);
        mg_proto_stream_tick(&gp);

        if (!reply2_started && now_ms >= reply2) {
            /* 6. The next reply: a fresh start_streaming, then interleaved again. */
            reply2_started = true;
            reply2_at = (int)now_ms;
            start_reply();
        }

        /* 5. Video, and requested keyframes. */
        if (app.audio_on || app.audio_cut || app.selected != mg_data_type_video_h264) {
            select_type(mg_data_type_video_h264);
            if (app.audio_cut && select_video_t < 0) select_video_t = (int)now_ms;
        }
        app_video_round();

        /* The talk press lands while a frame is half sent and the app is
         * mid-write of its audio: 2. the device drops the stream, 3. the
         * writes already in flight still arrive. */
        bool press_now = !pressed && now_ms >= press_after && app.frame_sent < app.frame_len && app.frame_sent > 0;
        if (app.audio_on) {
            select_type(mg_data_type_audio_pcm);
            for (int k = 0; k < AUDIO_WRITES; k++) {
                if (k == 1 && press_now) {
                    pressed = true;
                    press_t = (int)now_ms;
                    discarded_before = mg_proto_rx_discarded(&gp);
                    if (client_end) {
                        /* The app ends the reply itself: stop_streaming drop. */
                        uint8_t stop[] = { mg_command_stream_audio, mg_stream_audio_stop_streaming,
                                           mg_stream_audio_buffer_drop };
                        ctl(stop, sizeof(stop));
                        app.audio_on = false;
                        break;
                    }
                    mg_proto_claim_turn(&gp, true);
                    if (damage) {
                        /* The old routing moved Data to the video when the device
                         * ended the reply: the stray audio went into the
                         * video ring, mid-frame. */
                        uint8_t m[2] = { mg_command_change_data_type, mg_data_type_video_h264 };
                        ctl(m, 2);
                    }
                }
                if (pressed && press_t == (int)now_ms && !client_end) {
                    /* in flight: the app hasn't heard the stop yet */
                    data(pcm, AUDIO_WRITE);
                    in_flight += AUDIO_WRITE;
                    continue;
                }
                if (!app_audio_write()) break;
            }
            if (client_end && pressed && press_t == (int)now_ms) {
                /* its next video goes without change_data_type: Data went
                 * back to the video when the client ended the reply */
                app.selected = mg_data_type_video_h264;
                app_video_round();
            }
        }
        if (pressed && !released && now_ms >= (uint32_t)press_t + 400) {
            released = true;
            mg_proto_claim_turn(&gp, false);   /* the talk key up */
        }
        decode_pump();
        if (ask_pending) {
            ask_pending = false;
        }
    }

    uint32_t discarded = mg_proto_rx_discarded(&gp) - discarded_before;
    printf("{\"scenario\": \"%s\", \"press_t\": %d, \"select_video_t\": %d, \"reply2_t\": %d, \"in_flight\": %u, "
           "\"discarded\": %u, \"resyncs\": %u, \"rx\": %d, \"shown\": %d, \"matched\": %d, \"mismatched\": %d, "
           "\"fast_keyframes\": %d, \"dec_errors\": %d, \"dropped\": %d, \"asks\": %d, \"trouble_t\": %d, "
           "\"recovered_t\": %d, \"keyframes_to_recover\": %d, \"bad_after_recovery\": %d, \"shown_reply2\": %d, "
           "\"matched_reply2\": %d, \"frames_sent\": %u, \"ps_keyframe\": %d, \"ps_keyframe_result\": %d, "
           "\"ps_same_size_p_result\": %d, \"ps_other_size_p_result\": %d, \"after_skip_result\": %d, "
           "\"after_skip_asked\": %s, \"behind_at\": %d, \"behind_asked\": %s}\n",
           scenario, press_t, select_video_t, reply2_at, in_flight, discarded, gvs.resyncs, st.rx, st.shown,
           st.matched, st.mismatched, st.fast_keys, st.dec_errors, st.dropped, st.asks, st.trouble_at,
           st.recovered_at, st.keys_to_recover, st.bad_after_recovery, st.shown_reply2, st.matched_reply2,
           app.number, inject_key_frame, inject_result[0], inject_result[1], inject_result[2], skip_result,
           skip_asked ? "true" : "false", behind_at, behind_asked ? "true" : "false");
    muse_vdec_close(dec);
    mg_proto_disconnect(&gp);
    return 0;
}
