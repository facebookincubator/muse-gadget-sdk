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

#include "mg_codec.h"
#include "mg_config.h"
#include "mg_lct.h"
#include "mg_link.h"
#include "mg_loud1s.h"
#include "mg_play.h"
#include "mg_press_src.h"
#include "mgcommands.h"
#include "mg_queue.h"
#include "mg_session.h"
#include "mg_token_proof.h"
#include "mg_video.h"
#include "mg_ui.h"

/*
 * The device side of the musegadgets BLE protocol (protocols/mgcommands.h)
 * for one connection, with no BLE
 * stack in it: the stack glue (mg_ble.c) feeds it writes and subscription
 * changes, and it answers through mg_proto_ops_t. The host tests drive it the
 * same way. Not thread-safe: the caller serializes every call.
 */

typedef enum {
    MG_CH_CONTROL,
    MG_CH_DATA,
    MG_CH_COUNT,
} mg_ch_t;

/* Throughput test counters: the report's bytes, packets, first-to-last ms and gaps. */
typedef struct {
    uint32_t bytes, packets, gaps, next_seq;
    uint32_t first_ms, last_ms;
} mg_tp_count_t;

/* A test packet: a uint32 sequence number, then (seq + i) & 0xFF. */
void mg_tp_fill(uint8_t *buf, size_t len, uint32_t seq);
/* Counts one packet; false if it is too short to carry a sequence number. */
bool mg_tp_account(mg_tp_count_t *c, const uint8_t *buf, size_t len, uint32_t now_ms);
/* [device_action, throughput_test, 0, bytes, packets, ms, gaps]: 19 bytes. */
size_t mg_tp_report(const mg_tp_count_t *c, uint8_t out[19]);

/* The settings the device keeps across restarts. */
typedef struct {
    uint16_t buzz_freq_hz;
    uint16_t buzz_ms;
    uint8_t buzz_volume;
    uint8_t codec;          /* audio_codec (7): the user's turns outside calls; kept in NVS */
    bool queue_enabled;
    uint16_t capture_rate;  /* capture_sample_rate (13), kept in NVS */
    uint16_t capture_kbps;  /* capture_bitrate_kbps (14), kept in NVS */
} mg_settings_t;

void mg_settings_defaults(mg_settings_t *s);

#include "mg_battery.h"   /* report_battery_status's fields */

#include "mg_subs.h"   /* Live call subtitles' types */


typedef struct {
    void *ctx;
    /* One notification on a characteristic; false if it couldn't go. */
    bool (*notify)(void *ctx, mg_ch_t ch, const uint8_t *data, size_t len);
    void (*disconnect)(void *ctx);
    /* start_mic / stop_mic from the client. */
    void (*capture)(void *ctx, bool start, const mg_audio_format_t *fmt);   /* fmt: start only (else NULL) */
    /*
     * The capture's outbox (mg_capq_t): storage for the audio a capture holds
     * while the link can't take it, up to want bytes (*got: what it gives),
     * freed with capq_free once everything has gone. NULL, or nothing
     * given: the built-in MG_CAPQ_FALLBACK bytes.
     */
    void *(*capq_alloc)(void *ctx, size_t want, size_t *got);
    void (*capq_free)(void *ctx, void *buf);
    /* The stack has buffers to spare for held audio now (NULL: always).
     * Control messages never ask: they're what the reserve is for. */
    bool (*data_room)(void *ctx);
    /* set_mic_gain's level (1-100) for this connection's captures, or 0: the
     * connection ended, back to the device's own gain (the user's Settings
     * value or the tuned one). Per connection, never stored (mgcommands.h). */
    void (*mic_gain)(void *ctx, uint8_t gain);
    /* A persisted setting changed: store mg_proto_settings(). */
    void (*settings_changed)(void *ctx);
    uint32_t (*now_ms)(void *ctx);
    /* The same clock in 64 bits, never wrapping (now_ms wraps every 49.7 days): the press window's
     * (mg_proto_press). NULL: now_ms. */
    int64_t (*mono_ms)(void *ctx);
    /* Fills out with random bytes (the platform CSPRNG); false on failure. */
    bool (*random)(void *ctx, uint8_t *out, size_t n);
    /* ---- playback ---- */
    /* A stream started or stopped: wake the playback task. */
    void (*play_kick)(void *ctx);
    /* A reply stream is starting (start_streaming taken), called before its
     * first buffer_update: the link asks for active now, so the
     * request goes ahead of the answer. NULL: nothing. */
    void (*stream_starting)(void *ctx);
    /*
     * mg_setting_parameter_speaker_volume, for boards with a speaker (both
     * NULL: the setting is unsupported). 0-100, 0 = speaker off; the board
     * persists it.
     */
    uint8_t (*speaker_volume)(void *ctx);
    void (*set_speaker_volume)(void *ctx, uint8_t volume);
    /* haptic_play (15) or haptic_stop (17): the whole device_action params
     * (a[0] the action). Returns 0 once handled (the statuses go through
     * mg_proto_haptic_status), else the error code to send. */
    uint16_t (*haptic)(void *ctx, const uint8_t *a, size_t n);
    /* mg_command_assistant_state from the client (and idle at disconnect), for cfg.assistant. */
    void (*assistant_state)(void *ctx, uint8_t state);
    /*
     * ---- token proof (mg_command_token_proof; NULL: unsupported) ----
     * proof_key copies the proof key K that Link setup stored; false if the
     * device holds none. proof_clear erases the tokens and K and returns the
     * device to setup (it may end the connection); the protocol has already
     * answered result 1.
     */
    bool (*proof_key)(void *ctx, uint8_t k[MG_TOKEN_PROOF_KEY_LEN]);
    /* The device is set up (a key, or Link setup's marker without one). Set up with no key: no owner, so the
     * owner-only commands are refused (owner_ok). NULL: set up is having the key. */
    bool (*set_up)(void *ctx);
    void (*proof_clear)(void *ctx);
    /* Optional, for the sealed clear (mg_session_clear): starts the same erase where it runs, and returns true;
     * the board then calls mg_proto_clear_done() with its result, which the device session answers ([1] or
     * [0]) before the board restarts. False (or NULL): proof_clear runs it here, answered [1]. */
    bool (*proof_clear_async)(void *ctx);
    /*
     * ---- video (mg_command_stream_video; cfg.video NULL: unsupported) ----
     * video_begin: a stream with these parameters starts (the vstream is
     * already empty and active); false refuses it as busy. video_end: it
     * ended, from either side or with the connection.
     */
    bool (*video_begin)(void *ctx, const mg_video_params_t *vp);
    void (*video_end)(void *ctx);
    /* After the device ended a stream, Data selected for it was discarded
     * until the client's change_data_type (mgcommands.h): `bytes`
     * of it, once per episode, at that change_data_type. NULL: not told. */
    void (*rx_held)(void *ctx, uint32_t bytes);
    /*
     * ---- live call (mg_command_live_call; cfg.live_call false: unsupported) ----
     * The call's state (mg_live_call_state_t) as the client reports it, and
     * idle when a client with a call goes away.
     */
    void (*live_call)(void *ctx, uint8_t state);
    /*
     * ---- UI input (mg_command_ui_input; cfg.ui_input 0: unsupported) ----
     * hid_map: the client's map for the gadget's HID (parsed and checked);
     * returns the mg_hid_status_flags_t and sets *mode to the mode in use.
     * NULL: hid_map is refused as unsupported.
     */
    uint8_t (*hid_map)(void *ctx, const mg_hid_map_t *m, uint8_t *mode);
    /* hid_key: press and release a consumer-control key (usage) on the HID. False: no HID host. NULL: unsupported. */
    bool (*hid_key)(void *ctx, uint16_t usage);
    /* mark: a live call's step (mg_lct_point_t) happened, for its timeline. May be NULL. */
    void (*mark)(void *ctx, uint8_t point);
    /*
     * ---- subtitles (mg_command_subtitle; cfg.subtitles NULL: unsupported) ----
     * Each phone -> device message, checked and parsed (words point into the
     * message: copy what you keep).
     */
    void (*subtitle)(void *ctx, const mg_subs_event_t *ev);
    /*
     * ---- clock (device_action set/get_unix_time, mgcommands.h, Clock) ----
     * NULL: the device has no clock (both answered unsupported). clock_set
     * gets a checked unix_time_ms (never INT64_MIN) and the offset and flags
     * when the message carried them (keep the last ones otherwise); store:
     * the offset and flags may be kept across restarts (not set up, or this
     * connection's token proof matched), else they apply in RAM only.
     * clock_get fills the current time (INT64_MIN: never synced), offset and
     * flags.
     */
    void (*clock_set)(void *ctx, int64_t unix_ms, bool has_offset, int16_t offset_min, bool has_flags, uint8_t flags,
                      bool store);
    void (*clock_get)(void *ctx, int64_t *unix_ms, int16_t *offset_min, uint8_t *flags);
    /*
     * ---- subtitles ready (optional) ----
     * False while the device can't draw subtitles (its band's buffers didn't
     * allocate): mg_command_subtitle is then unlisted and refused. NULL: the
     * configured cfg.subtitles is always drawn.
     */
    bool (*subtitles_ready)(void *ctx);
    /*
     * ---- battery and power (device_action 8/9 and 10) ----
     * battery_status fills the latest reading; false: none yet (answered
     * busy). NULL: no battery to report (8 unsupported, unlisted).
     * power_off turns the device off (from another task: return at once).
     * NULL: it can't power off from software (10 unsupported, unlisted).
     */
    bool (*battery_status)(void *ctx, mg_battery_status_t *out);
    void (*power_off)(void *ctx);
    /* Whether power_off works now (some boards only on battery). NULL: always. */
    bool (*power_off_ready)(void *ctx);
    /* device_action reboot in a recovery mode with
     * mg_recovery_flag_restart: disconnect cleanly and restart into the
     * normal firmware (from another task: return at once). */
    void (*reboot)(void *ctx);
    /*
     * ---- device session (mg_command_session; cfg.session NULL: unsupported) ----
     * session_credentials fills the credential Link setup stored (false: no token);
     * session_store_tokens stores a refreshed pair and its access_expires_at
     * (0: none) in one commit; session_credentials hands the stored one back
     * (mg_session_creds_t.access_expires_at).
     * session_state: the client reported the VM session (mg_session_state_t),
     * or it ended with a new hello or the connection. session_message: a
     * whole message from the VM waits for mg_proto_session_take().
     */
    bool (*session_credentials)(void *ctx, mg_session_creds_t *c);
    bool (*session_store_tokens)(void *ctx, const char *access_token, const char *refresh_token,
                                 uint64_t access_expires_at);
    void (*session_state)(void *ctx, uint8_t state, uint16_t epoch);
    /* The device session can't hand out credentials now (a re-setup is
     * committing): a hello is refused busy (the VM session) or waits (the
     * credentials-only one), and the proof is kept. NULL: never. */
    bool (*session_busy)(void *ctx);
    /* The gadget holds no Wi-Fi credentials, so it can't refresh its own
     * token pair: request_status's caps carry mg_session_cap_client_refreshes,
     * asked at each request_status. NULL:
     * the VM session's cfg.client_refreshes (BLE-only builds hold no Wi-Fi),
     * or none. */
    bool (*client_refreshes)(void *ctx);
    void (*session_message)(void *ctx);
    /* The device session refused a client's data message (a sealed
     * session's plain data, a wrong counter or tag; mg_session_ops_t
     * data_refused): why, and its total_len. For the log. NULL: not told. */
    void (*session_data_refused)(void *ctx, const char *why, uint32_t len);
    /*
     * ---- link modes (settings link_mode 16 and link_params 17; mgcommands.h
     * "Link modes"). All three NULL: neither is listed. ----
     * link_mode: the effective mode (mg_link_mode_t). set_link_mode: the
     * app's write; false if the value isn't a mode (invalid_value).
     * link_params: the parameters in effect, 6 bytes (interval, latency,
     * timeout, little-endian; all 0 before the controller reported any).
     * Per connection: nothing is stored. The device reports link_mode with
     * mg_proto_report_setting() on every effective change.
     */
    uint8_t (*link_mode)(void *ctx);
    bool (*set_link_mode)(void *ctx, uint8_t mode);
    void (*link_params)(void *ctx, uint8_t out[6]);
} mg_proto_ops_t;



typedef struct {
    mg_queue_t *queue;           /* the offline clip queue, or NULL */
    const mg_subs_caps_t *subtitles; /* Live call subtitles drawn (mg_command_subtitle), or NULL */
    mg_play_t *play;             /* playback (the board has a speaker), or NULL */
    bool haptics;                /* a vibration motor: haptics_enabled and ptt_buzz_* */
    /* Haptics: setting 15's four bytes (kind, flags, min_on_ms, strength
     * steps) where the board runs the haptics core (haptic_play 15 /
     * haptic_stop 17 through ops.haptic); NULL: only the talk buzz. */
    const uint8_t *haptic_actuator;
    bool assistant;              /* a display or status light: assistant_state */
    const mg_video_caps_t *video; /* video streaming, or NULL */
    muse_vstream_t *vstream;       /* its receive ring (with video) */
    /* Streams to a set-up gadget (start_streaming) without the token proof on a connection whose bench override is
     * on (automation builds; mgcommands.h, Token proof). A release build takes them only after the proof. */
    bool bench_streams;
    bool live_call;              /* the user can start a live call: mg_command_live_call */
    /* mg_command_ui_input: a bit (1 << mg_ui_input_feature_t) for each part
     * offered (touch events, the HID modes); 0: not offered. */
    uint8_t ui_input;
    /* A recovery or SAFE mode,
     * mg_device_mode_t (0: normal). request_status answers change_data_type,
     * the declaration [10 FE 0A mode reason flags], then [10 request_status
     * device_action]; everything else is refused with mg_error_code_recovery
     * (token_proof included: never a response), except device_action
     * request_battery_status and, with mg_recovery_flag_restart, reboot. The
     * device starts nothing itself (no gesture or capture). */
    uint8_t recovery_mode, recovery_reason, recovery_flags;
    /* The mic delivers 24 kHz, so LC3 captures list
     * 24000 after 16000 (capture_sample_rate, start_mic's rate). false: 16 kHz
     * captures only (the bitrate is still the client's to choose). */
    bool capture_24k;
    /* mg_command_session: the device's VM session over BLE (its buffer and
     * sizes), or NULL. Needs the session ops and the token proof. */
    const mg_session_config_t *session;
    /* Without the VM session (cfg.session NULL): a credentials-only device
     * session (portable/mg_device_session layer 2), the
     * Wi-Fi gadgets': hello, sealed credentials and credentials_update, every
     * VM-session part refused; caps sealed only (they refresh their own pair
     * over Wi-Fi). Needs the token proof and session_credentials and
     * session_store_tokens. */
    bool creds_session;
    /*
     * Mic output goes only to
     * a central that passed this connection's token proof. Set on every
     * firmware build (mg_ble.c, whether or not the platform has a proof key:
     * without one nothing can match, so nothing goes). A capture before the
     * proof is held whole in the capture's outbox and sent in order once the
     * proof matches; a full hold stops the capture and keeps what it holds;
     * a failed proof, a gadget with no proof key, or the link's end discards
     * it. Receive paths aren't gated here, except streams to a set-up
     * gadget (start_streaming), which need the token proof as well
     * (bench_streams). The bench switch (mg_proto_set_output_bench,
     * the console's bench.out, automation, off at every boot) lifts the gate
     * for a Mac bench client. false: no gate (host harnesses that model no
     * proof).
     */
    bool output_proof;
    /* start_mic's press rule (mgcommands.h, start_mic): a release build of a set-up device takes a client's
     * start_mic only within MG_PRESS_WINDOW_MS of a press the device itself detected (mg_proto_press), besides the
     * token proof. false: the proof alone (automation and test builds, host harnesses). */
    bool press_gate;
    /* A release build of a set-up gadget answers haptic_play proof_required until this connection's proof matched
     * (haptic_stop always runs). false: bench and automation builds, which run whole flows with no human; host
     * harnesses that model no proof. */
    bool proof_inputs;
} mg_proto_config_t;

struct mgds;
struct mgds_port;
typedef struct mg_creds_cache mg_creds_cache_t;   /* mg_proto.c */

/*
 * The capture's outbox (mgcommands.h, Push-to-talk; on the Watcher
 * a turn on a sleep link lost half its frames and its stop_mic, and the phone
 * waited 120 s for the end): everything a capture sends, in order, through
 * one queue: [gesture, button_down], change_data_type, the audio, [gesture,
 * button_up], [stop_mic]. Audio is held in a buffer (PSRAM: up to
 * CONFIG_MUSE_GADGET_BLE_CAPTURE_HOLD_S of the format) and drained as fast as
 * the link takes it; control messages wait in place between the audio and
 * are never dropped, only retried (every pump: the voice task's each 20 ms
 * chunk, the worker's 5 ms rounds) until they go or the link drops. Only
 * when the buffer overflows is the oldest audio dropped, whole frames.
 * Nothing in it ever blocks the voice task.
 */
#define MG_CAPQ_CTL_MAX 12        /* control messages waiting: four a turn, three turns */
#define MG_CAPQ_CTL_BYTES 12      /* the longest: change_data_type with its format (10) */
#define MG_CAPQ_FALLBACK 512      /* the built-in buffer when nothing else is given */

/*
 * A capture's level for its summary ("did the mic hear speech",
 * answered from the gadget alone), as the Wi-Fi path's `muse_voice:
 * recorded ... rms ... peak ... floor ... clipped` line measures it: rms over
 * every sample, peak, floor (the quietest 20 ms window's rms) in dBFS, and
 * samples at |v| >= 32000. Integer sums per sample (no floating point on the
 * voice task: the C6 has no FPU); dB only at the end.
 */
#define MG_METER_WINDOW 320               /* 20 ms at 16 kHz: the floor's window */
#define MG_METER_FRAME (MG_METER_WINDOW / 2)   /* 10 ms at 16 kHz: the loudest second's frames */
#define MG_METER_CLIP 32000
typedef struct {
    uint64_t acc;                 /* sum of squares, every sample */
    uint32_t frames;
    int32_t peak;
    uint32_t clipped;
    uint64_t win_acc;             /* the window being summed */
    uint32_t win_n;
    uint64_t win_half;            /* win_acc at the window's first 10 ms (its first frame) */
    uint64_t floor_acc;           /* the quietest whole window's sum (UINT64_MAX: none yet) */
    mg_loud1s_t loud;             /* the loudest 1 s window, from each 10 ms frame (mg_loud1s.h) */
} mg_capture_meter_t;

typedef struct {
    bool valid;
    bool loud_valid;              /* loud1s_db measured (a ring was given) */
    uint32_t ms;                  /* the capture's length */
    float rms_db, peak_db, floor_db;   /* dBFS, -100 at the least */
    float loud1s_db;              /* the loudest 1 s window's RMS (the whole, under 1 s), dBFS */
    uint32_t clipped;
} mg_capture_level_t;

/* The loudest second's ring (MG_LOUD1S_RING_BYTES, PSRAM where it can be;
 * NULL: not measured), kept by every reset after. */
void mg_capture_meter_set_ring(mg_capture_meter_t *m, uint32_t *ring);
void mg_capture_meter_reset(mg_capture_meter_t *m);
void mg_capture_meter_add(mg_capture_meter_t *m, const int16_t *pcm, size_t n);
void mg_capture_meter_level(const mg_capture_meter_t *m, uint32_t rate_hz, mg_capture_level_t *out);

typedef struct {
    uint32_t at;                  /* the audio position (mg_capq_t.wseq) it goes at */
    uint32_t queued_ms;           /* when it was queued (an end's wait, for the log) */
    uint16_t fb;                  /* frame bytes of the audio after it (change_data_type), 0: unchanged */
    uint8_t len;
    bool end;                     /* stop_mic: the end of an utterance */
    bool passed;                  /* dropped audio went past it: its fb already applies */
    bool held;                    /* a held turn's (mg_proto_turn_hold): waits until it's released or dropped */
    uint8_t msg[MG_CAPQ_CTL_BYTES];
} mg_capq_ctl_t;

typedef struct {
    uint8_t *buf;                 /* the audio ring (NULL until a capture needs it) */
    size_t cap;
    bool owned;                   /* buf came from ops.capq_alloc */
    uint32_t wseq, rseq;          /* audio bytes queued and taken (sent or dropped) since connect */
    size_t rpos;                  /* rseq's place in buf */
    uint16_t wfb, rfb;            /* frame bytes of the audio being queued and taken */
    uint8_t nctl;
    mg_capq_ctl_t ctl[MG_CAPQ_CTL_MAX];
    /* The utterance being drained: bytes sent, frames dropped, most bytes held. */
    uint32_t utt_sent, utt_dropped, utt_held_max;
    /* Ends delivered since boot, and the last one's numbers (mg.voice logs them). */
    uint32_t ends_sent;
    uint32_t end_wait_ms, end_sent_bytes, end_dropped, end_held_max;
    uint16_t end_fb, end_frame_us;
    uint32_t ctl_lost;            /* a control message that found the queue full (never expected) */
    /* Levels (mg_capture_level_t): the next end's (mg_proto_capture_level),
     * the ends queued in order, and the last end sent's. */
    mg_capture_level_t next_level, end_level;
    mg_capture_level_t levels[3];
    uint8_t nlevels;
    /* The bench guard's held turn (mg_proto_turn_hold, ptt.minrise): its
     * control messages (held) and its audio from gate_at on wait here, none
     * sent, until it rises gate_db over its floor (released) or ends
     * (dropped: none of it ever goes). */
    bool gate;
    uint8_t gate_db;
    uint16_t gate_wfb;            /* wfb before the turn, back at a drop */
    uint32_t gate_at;             /* wseq at the hold: the turn's first audio byte */
    uint32_t gate_dropped;        /* utt_dropped at the hold, back at a drop */
    float gate_rise;              /* the last rise checked (dB over the floor), for the log */
    /* The proof hold (cfg.output_proof): while the central hasn't passed
     * this connection's token proof nothing in here goes, and audio that
     * doesn't fit is refused rather than the oldest dropped (full: the
     * capture stops, what's held is kept). Counts for the status line. */
    bool proof_full;              /* the hold filled before the proof: no more audio until it's sent or discarded */
    uint32_t proof_fills;         /* holds that filled before their proof */
    uint32_t proof_releases;      /* holds sent at a proof's match (or the bench switch) */
    uint32_t proof_discards;      /* holds discarded (a failed proof, no proof key) */
    uint32_t proof_discarded;     /* ...their audio bytes */
    /* The held live turn (mg_proto_held_turn_begin): a press during the connect burst. Its
     * control messages (held) and its audio from ht_at wait, none sent, until the client is proven
     * and ready (released: it goes live) or it's taken for the clip queue. */
    bool held_turn;
    bool ht_spill;                /* it goes to the clip queue now, whatever the time (the link dropped) */
    bool ht_voice;                /* the voice task is still capturing it (it takes it; else the worker) */
    uint32_t ht_at;               /* wseq at its start: its first audio byte */
    uint16_t ht_wfb;              /* wfb before it, back once it's taken */
    uint32_t ht_ms;               /* when it started (now_ms): the deadline counts from here */
    uint64_t ht_unix_ms;          /* ...on the wall clock, for its clip (0: not set) */
    uint32_t ht_live, ht_queued;  /* held turns that went live, and that went to the clip queue */
    mg_audio_format_t ht_fmt;     /* its audio's format (type 0 until its change_data_type is queued) */
    /* The held turn's staleness bound (MG_OUTPUT_STALE_MS): a capture that ended while held (or was still held when a
     * re-proof began) is discarded at stale_at unless released first. */
    bool stale_armed;
    uint32_t stale_at;
    uint32_t proof_stale, proof_stale_bytes;   /* holds discarded as stale, and their bytes */
    uint8_t fallback[MG_CAPQ_FALLBACK];
} mg_capq_t;

/* The longest Control reply the device builds (a gadget's
 * reply slot silently dropped an over-long capabilities reply): one
 * notification at the protocol's MTU floor (MG_MIN_ATT_MTU - 3, 97 B). Every
 * reply buffer in mg_proto.c is checked against it at build time
 * (_Static_assert beside each), and test_reply_sizes builds every typed list
 * at its maximum (stream_video's capabilities with still, about
 * 75 B) against it. A get_settings answer longer than this goes in pieces. */
#define MG_CTL_REPLY_MAX (MG_MIN_ATT_MTU - 3)

/* Each typed list's longest form, every feature and option on: the
 * buffers in mg_proto.c are these sizes, each asserted against
 * MG_CTL_REPLY_MAX there; test_reply_sizes builds every list at them. */
#define MG_CAPTURE_CODECS_MAX 2   /* start_mic's codecs (mg_codec_list): SBC, LC3 */
#define MG_CAPTURE_RATES_MAX 2    /* MG_CODEC_RATE, MG_CODEC_RATE_HIGH */
#define MG_PLAY_CODECS_MAX 8      /* stream_audio's codecs (mg_play_codecs' cap; 3 today) */
#define MG_PLAY_RATES_MAX 8       /* stream_audio's sample rates (mg_play_rates' cap; 6 today) */
#define MG_SETTING_PARAM_LAST mg_setting_parameter_client_app   /* the last setting mgcommands.h defines */
/* [supported_features, 6 commands always, 10 optional] */
#define MG_REPLY_FEATURES_MAX (1 + 6 + 10)
/* [10 FE 01, codecs] */
#define MG_REPLY_START_MIC_MAX (3 + MG_CAPTURE_CODECS_MAX)
/* [10 FE 08, (codec, count, count x (rate, min, max))...] */
#define MG_REPLY_FORMATS_MAX (3 + MG_CAPTURE_CODECS_MAX * (2 + MG_CAPTURE_RATES_MAX * 6))
/* [10 FE 41, every setting 0..MG_SETTING_PARAM_LAST] */
#define MG_REPLY_SETTINGS_MAX (3 + MG_SETTING_PARAM_LAST + 1)
/* [10 FE 1E, battery request+report, power_off, throughput_test, clock x3, haptic x2] */
#define MG_REPLY_ACTIONS_MAX (3 + 9)
/* [10 FE ui_input, features 1..7] */
#define MG_REPLY_UI_INPUT_MAX (3 + 7)
/* stream_audio report_capabilities: [cmd, sub, count, codecs (2 + n), buffer_size 5,
 * sample_rates (2 + 4n), max_bitrate 3, max_channels 2, flush 2, live_depth 3] */
#define MG_REPLY_AUDIO_CAPS_MAX (3 + 2 + MG_PLAY_CODECS_MAX + 5 + 2 + 4 * MG_PLAY_RATES_MAX + 3 + 2 + 2 + 3)
/* stream_video report_capabilities, every capability mgcommands.h defines,
 * value sizes from its text: [cmd, sub, count], codecs (h264, vp8),
 * display, max_size, max_fps, buffer_size, suggested, l2cap_psm,
 * playout_delay, tiles, tile_split, ui, ui_still, flags, still (its image
 * codecs). No ESP32 build lists still today; the reply is sized for it anyway. */
#define MG_VCAP_STILL_CODECS_MAX 1   /* mg_data_type_image_jpeg */
#define MG_REPLY_VIDEO_CAPS_MAX                                                                                  \
    (3 + (2 + 2) + (1 + 5) + (1 + 4) + (1 + 1) + (1 + 4) + (1 + 7) + (1 + 2) + (1 + 2) + (1 + 1) + (1 + 1) +    \
     (1 + 10) + (1 + 1) + (1 + 1) + (1 + 13 + MG_VCAP_STILL_CODECS_MAX))

/* The Control reply outbox (mg_proto.c, send_control()): one-shot replies the
 * stack refused, in order, behind the token proof answer, each kept whole
 * ([uint16_t len, bytes] records, oldest first, in MG_CTLQ_RING bytes). The
 * worst request_status is 11 lines (change_data_type, the command list,
 * 10 FE 89, start_mic's codecs, change_data_type's formats, settings, device
 * actions, UI input, a pending video request: 120 B), with the proof result and spare: 16 records. A reply
 * longer than the link's ATT payload (MTU - 3: before the MTU exchange, or
 * under MG_MIN_ATT_MTU) waits whole, and everything after it, until the MTU
 * rises; NimBLE would cut it short (a truncated reply parses as valid). A
 * reply that finds no room is counted (dropped, on the console's status
 * line; never expected). */
#define MG_CTLQ_MAX 16
#define MG_CTLQ_RING 1024
typedef struct {
    uint8_t buf[MG_CTLQ_RING];   /* [uint16_t len LE, len bytes] records, oldest first */
    uint16_t used;               /* bytes of buf in use */
    uint8_t n, most;             /* records waiting; most: the most ever (a bench line) */
    uint16_t most_bytes;         /* the most bytes ever waiting (a bench line) */
    uint32_t dropped;            /* replies that found no room (never expected) */
    uint32_t held_mtu;           /* replies that waited for the MTU exchange (longer than MTU - 3) */
} mg_ctlq_t;
_Static_assert(MG_CTLQ_RING <= UINT16_MAX, "mg_ctlq_t.used is 16-bit");
_Static_assert(MG_CTLQ_RING >= MG_CTLQ_MAX * (2 + 9) && MG_CTLQ_RING >= 4 * (2 + MG_CTL_REPLY_MAX),
               "the outbox holds the worst request_status and four of the longest replies whole");

/* What the console's status line shows of Control's queues (counts only):
 * mg_proto_reply_stats(). */
typedef struct {
    uint32_t dropped;            /* replies with no room in the outbox */
    uint32_t held_mtu;           /* replies (and token proof answers) that waited for the MTU exchange */
    uint32_t link_refused;       /* notifications refused for being longer than MTU - 3 (never expected) */
    uint32_t cmd_direct;         /* client commands that ran past the full command queue */
    uint32_t capture_lost;       /* capture controls past the full capture outbox */
    uint32_t session_dropped;    /* device session answers with no room in its queue */
    uint32_t touch_dropped;      /* touch events the stack refused (sent now or never) */
    uint32_t nus_dropped;        /* log mirror lines dropped for want of room (mg_nus.h; filled by mg_ble) */
    uint32_t host_closed;        /* links the BLE host closed itself, not our code (mg_link_close.h; filled by mg_ble) */
    uint8_t waiting, most;       /* replies waiting now; the most ever */
} mg_reply_stats_t;

/* Client commands held while replies wait (mgcommands.h:
 * replies keep their order and are never dropped; a gadget's mic
 * error under request_status bursts during a talk hold). While the reply
 * outbox holds anything, a Control command that answers through it waits
 * here, in order, and runs once the outbox is empty, so one command's answer
 * (11 lines at most) always fits the outbox. A token proof and the device
 * session's messages never wait (their answers have their own outboxes). A
 * command that doesn't fit is run at once (counted, never expected). */
#define MG_CMDQ_BYTES 192
typedef struct {
    uint8_t buf[MG_CMDQ_BYTES];  /* [len, command...] records, oldest first */
    uint16_t used;
    uint8_t n, most;             /* commands waiting; the most ever (a bench line) */
    uint32_t direct;             /* commands that didn't fit and ran at once (never expected) */
    bool running;                /* running one now: its own replies don't run the next */
} mg_cmdq_t;

typedef enum {
    MG_PROOF_IDLE,
    MG_PROOF_RESPONDED,          /* response sent, waiting for confirm */
    MG_PROOF_MATCHED,            /* result 1 on this connection */
} mg_proof_state_t;

typedef struct {
    mg_proto_config_t cfg;
    mg_proto_ops_t ops;
    mg_settings_t settings;

    /* ---- this connection ---- */
    bool connected;
    bool setup_changing;   /* mg_proto_proof_reset running: a sealed clear the old session took is dropped */
    uint16_t mtu;
    bool subs_caps_sent;         /* subtitle capabilities answered on this connection (resent if the MTU changes) */
    bool sub[MG_CH_COUNT];
    bool ptt_enabled;
    /* A new token proof's challenge on this connection (an iOS
     * relaunch can keep the link up) clears push-to-talk until the proof is
     * valid; an enable the client sends meanwhile is held for it. */
    bool ptt_held;               /* cleared by a challenge, the proof not valid yet */
    bool ptt_asked;              /* ...and the client asked for it meanwhile */
    bool ptt_cleared_note;       /* a challenge cleared it: mg_ble logs it once */
    uint8_t mic_gain;            /* the client's set_mic_gain level, 0: none (the device's own gain) */
    bool haptics;
    uint8_t start_mic_codec;     /* codec of the last start_mic that named one, else 0 */
    uint8_t data_type;           /* the type last announced (or the default) */
    mg_audio_format_t data_fmt;  /* the audio format last labeled (data_type an audio codec) */
    bool live;                   /* an utterance is streaming */
    mg_capq_t capq;              /* what a capture sends, in order (mg_capq_t) */
    bool reports_state;          /* the client has sent assistant_state */
    /* Who is connected (client_app 19; client_name 18 is reserved):
     * per connection, empty at connect and disconnect; as it went on the wire
     * (the app's name, then its version, each a length byte, then UTF-8).
     * client_gen moves at every change, for the status page. */
    uint8_t client_app[2 + 2 * MG_CLIENT_APP_FIELD_MAX];
    uint8_t client_gen;

    struct {                     /* the token proof on this connection; wiped at connect and disconnect */
        uint8_t state;           /* mg_proof_state_t */
        uint8_t client_nonce[MG_TOKEN_PROOF_NONCE_LEN];
        uint8_t device_nonce[MG_TOKEN_PROOF_NONCE_LEN];
        uint8_t client_mac[MG_TOKEN_PROOF_MAC_LEN];   /* the confirm expected */
        bool nonces_held;        /* a match's nonces, not yet taken by a sealed hello */
    } proof;
    mg_ctlq_t ctlq;              /* the Control reply outbox (one-shot replies the stack refused) */
    /* speaker_volume reports, owed until one leaves on Control: the
     * level a client last saw (the last report the stack took, or its own set; -1 none yet, kept across
     * connections) and a device change not yet reported. */
    int16_t spk_seen;
    bool spk_owed;
    uint32_t spk_sent;           /* reports that left */
    mg_cmdq_t cmdq;              /* client commands waiting for the outbox to empty (mg_cmdq_t) */
    /* The challenge's answer the stack hasn't taken yet: it goes ahead of every other Control message, and once
     * it has waited MG_PROOF_ANSWER_MS it's replaced by token_proof busy so
     * the client asks again at once. */
    struct {
        uint8_t msg[2 + MG_TOKEN_PROOF_NONCE_LEN + MG_TOKEN_PROOF_MAC_LEN];
        uint8_t len;             /* 0: nothing waiting */
        bool busy;               /* it's the busy error now */
        uint32_t since_ms;
    } proof_out;

    bool ever_ready;             /* a client's session has been up since boot */

    /* The credentials-only device session (cfg.creds_session), allocated at
     * init: layer 2's state and port, and the credential while a hello
     * reads it (freed once nothing is owed). */
    struct mgds *creds;
    struct mgds_port *creds_port;
    mg_creds_cache_t *creds_cache;
    uint8_t *creds_in;           /* storage lent for an incoming credentials_update */

    bool audio_claimed;          /* the device's own sound (a chirp, a bench test, a saved note) has the audio path */
    bool audio_paused;           /* ...and a Live call's reply stream waits for it, its audio kept (mg_proto_play_held) */
    bool turn;                   /* a push-to-talk turn holds the speaker: playback waits, starts are taken */
    uint8_t stops_owed;          /* client stop_streaming answers owed for the playing stream (each answered once) */
    bool barge_paused;           /* a talk press's touch-down holds the reply until tap or turn (mg_proto_barge_pause) */
    uint32_t barge_ms;           /* ...since when (MG_PROTO_BARGE_PAUSE_MAX_MS) */
    bool claim_deferred;         /* the turn's claim waits for its release (a held turn, mg_proto_turn_hold) */
    uint8_t gate_data_type;      /* data_type and data_fmt before a held turn: what request_status says */
    mg_audio_format_t gate_data_fmt;   /* while it's held, and what a drop puts back */
    uint32_t ptt_nothing;        /* held turns that never rose, none of them sent (the status line's ptt_nothing) */
    uint32_t update_ms;          /* when the last buffer_update went */
    uint32_t update_avail;       /* ...and what it said */
    /* the most the client may have sent since start_streaming (the highest received + available a
     * buffer_update granted), and what came past it (dropped, counted). */
    uint32_t grant_limit;
    uint32_t credit_overrun_writes, credit_overrun_bytes;
    uint32_t video_overrun_writes, video_overrun_bytes;   /* the same for video */
    /* A live depth's view of the link (mg_play.h, Live depth): what's in flight, as the time from a
     * buffer_update that raised the client's allowed total (received + credit) to the first byte past the
     * total before it, smoothed (0: not measured yet; kept for the connection). The updates still waiting
     * for that byte: when each went and the total before it. */
    uint32_t flight_us;
    uint32_t flight_edge;        /* the highest total the stream's updates allowed */
    uint8_t flight_head, flight_n;
    struct {
        uint32_t at_ms, below;
    } flight[8];
    /* Control notifications handed to the stack, and the ones it reported
     * sent (mg_proto_control_sent): a reply stream's or video's new
     * buffer_update waits for the last to go, at most one per connection
     * event. */
    uint32_t ctl_sent, ctl_done, update_seq;
    /* Bench A/B (default off, so request answers stay immediate): with credit_coalesce, a
     * request_buffer_update during a live-depth reply is answered by the stream tick's next update (at most
     * 10 ms on, one in flight) rather than at once, so a client asking after every write doesn't double the
     * Control traffic (the C6 is CPU-bound in replies at ~85 audio writes a second). The console's
     * credit.coalesce=1|0 (RAM only, off at boot). */
    bool credit_coalesce;
    bool update_owed;
    /* The flush's buffer_update (mg_stream_audio_flush): the flush is applied where it arrives, on the BLE
     * host task in order with the Data writes around it (mg_ble.c mg_access); its update goes from the
     * worker's next stream tick (mg_proto_stream_tick), never from the host task. */
    bool flush_update_owed;
    uint32_t link_refused;       /* notifications longer than MTU - 3, refused rather than cut short (never expected) */
    uint32_t touch_dropped;      /* touch events the stack refused (sent now or never, counted) */

    bool transfer;               /* a read_clip download is under way */
    uint16_t transfer_index;
    uint32_t transfer_off;

    /* device_action throughput_test (mgcommands.h, Throughput test). */
    struct {
        uint8_t mode;            /* mg_throughput_test_t: 0 none, 1 sending, 2 receiving */
        uint8_t last;            /* the direction of the last test, for a late stop */
        bool armed;              /* receive: the client's change_data_type arrived */
        uint32_t start_ms, duration_ms;
        uint32_t seq;            /* send: next sequence number */
        uint32_t stalls;         /* send runs ended because the link took nothing for MG_TP_STALL_MS */
        mg_tp_count_t c;
    } tp;

    /* What the client's Data writes carry (mg_data_type_t): the stream that's
     * on, or with video and audio playback both on, the type the client's
     * last change_data_type chose. 0: nothing takes them. */
    uint8_t rx_type;
    /* Framed Data (mg_data_type_framed, mgcommands.h): after a change_data_type(framed) every Data write
     * starts with a type byte naming its stream; rx_type stays mg_data_type_framed. Ends with a
     * change_data_type to another type, when both streams have ended, or at disconnect. Counters since
     * connect (the console's framed? and the video stream's end line): framed_selects the selections
     * that turned it on (a repeat is a no-op), framed_wr_video / framed_wr_audio the framed writes per
     * stream, framed_disc the framed writes discarded (no open stream of their type; their bytes also in
     * rx_discarded), rx_switches every change_data_type to a plain type (the interleaving it replaces). */
    bool framed;
    uint32_t framed_selects, framed_wr_video, framed_wr_audio, framed_disc, rx_switches;
    uint32_t rx_discarded;       /* Data bytes for a type no open stream takes (dropped) */
    /* The device ended the stream Data was selected for: discarded until the
     * client's next change_data_type (rx_hold_bytes of it so far). */
    bool rx_hold;
    uint32_t rx_hold_bytes;

    /* mg_command_stream_video (mgcommands.h, Video streaming). */
    struct {
        bool active;             /* a stream is on: Data goes to cfg.vstream */
        uint8_t codec;           /* its mg_data_type_t (H.264 or VP8) */
        bool wanted;             /* the device's user asked for one (request_streaming 1) */
        uint32_t update_ms;
        uint32_t update_avail;
        uint32_t update_recv;    /* the received count it carried */
        uint32_t grant;          /* its received + available, what the client may have sent */
        bool granted;            /* an update went for this stream */
        bool overrun_asked;      /* a keyframe asked for after a write past the grant */
        uint32_t update_seq;     /* ctl_sent after it: the next waits for ctl_done to reach it */
        uint16_t update_shown;   /* ...and the shown count */
        uint16_t shown, dropped; /* reported in buffer_update */
        uint8_t decode_ms;
        uint8_t content;         /* mg_video_content_t of the stream that's on */
        bool have_shown;         /* a picture has been drawn: last_number and its time */
        bool got_bytes;          /* the stream's first Data bytes came (the call timeline) */
        uint16_t last_number;
        uint32_t last_shown_ms;
    } video;

    /* The last press the device itself detected (mg_proto_press), kept across connections, on the 64-bit clock
     * (ops.mono_ms): it opens start_mic for MG_PRESS_WINDOW_MS and never again (a 32-bit time would come
     * back around). */
    bool press_seen;
    int64_t press_ms;
    bool audio_got_bytes;        /* the playback stream's first Data bytes came (the call timeline) */

    /* mg_command_live_call (mgcommands.h, Live call). */
    struct {
        uint8_t state;           /* mg_live_call_state_t, as the client last reported it */
        bool pending;            /* the user started one and no client has answered yet */
        bool asked_here;         /* that pending start was sent to this connection (cleared at each link) */
        bool ended;              /* a call ended at ended_ms (its 3 s tail) */
        uint32_t ended_ms;
        bool mic_ignored;        /* a start_mic during a call went unanswered: its stop_mic is too */
        bool mic_taken;          /* a capture this client's start_mic began is on: its stop_mic is taken, and a
                                    call's start ends it */
        uint8_t fail_reason;     /* mg_live_call_failure_reason_t with the last failed */
        /* The owner's call is on (a state from a proven central, or the answer to this connection's own pending
         * start, turned it on); and the last state came from a central without this connection's proof (not that
         * answer). */
        bool owner_on;
        bool unproven;
        bool own_start;          /* the call on is the one this connection's own pending start began */
        bool held;               /* the UI holds the call behind the passcode keypad: its reply audio waits */
    } call;

    /* mg_command_ui_input (mgcommands.h, UI input). */
    bool touch_enabled;          /* per connection */
    uint8_t ui_ctl[8];           /* UI controls (mg_ui_control_t), as the client last said or the user set */
    uint8_t hid_mode, hid_flags; /* the last hid_status told (or to tell) */

    /* mg_command_session (mgcommands.h, Device session). */
    mg_session_t session;

    /* The proof gate (cfg.output_proof): the bench switch (RAM only, off at init,
     * kept across connections), what the last proof did to a hold for
     * mg_ble's log (MG_OUT_NOTE_*, taken once) and its bytes, and captures
     * refused for want of a proof key. */
    bool out_bench;
    /* The same switch from the Nordic UART (bench.out=1 there): this connection only,
     * cleared at connect and at disconnect (and off at init). */
    bool out_bench_conn;
    /* The data type from before a capture that began unproven (its own change_data_type, held, says it at
     * release): what request_status reports until the proof (nothing about a held capture,
     * its format or that one exists, reaches an unproven central). */
    bool pre_set;
    uint8_t pre_data_type;
    mg_audio_format_t pre_data_fmt;
    uint8_t out_note;
    uint32_t out_note_bytes;
    uint32_t out_no_key;
    uint32_t out_start_refused;  /* start_mic before the proof with no challenge outstanding (proof_required) */

} mg_proto_t;

/* A throughput send run ends when the link takes nothing for this long (a stalled or dead link): the worker
 * never spins on it, and tp.stalls counts it for the log. */
#define MG_TP_STALL_MS 2000u

/* What a token proof did to a held capture (mg_proto_take_output_note). */
#define MG_OUT_NOTE_NONE 0
#define MG_OUT_NOTE_RELEASED 1    /* matched (or the bench switch): sent in order */
#define MG_OUT_NOTE_DISCARDED 2   /* failed, or no proof key: discarded unsent */
#define MG_OUT_NOTE_QUEUED 3      /* a held live turn not proven and ready in time: to the clip queue */
#define MG_OUT_NOTE_STALE 4       /* not released within MG_OUTPUT_STALE_MS of the capture's end: discarded */

void mg_proto_init(mg_proto_t *p, const mg_proto_config_t *cfg, const mg_proto_ops_t *ops,
                   const mg_settings_t *settings);
/* The board's player couldn't start after mg_proto_init(): the protocol goes on without a speaker (cfg.play NULL).
 * Under the caller's lock. */
void mg_proto_drop_play(mg_proto_t *p);
const mg_settings_t *mg_proto_settings(const mg_proto_t *p);

void mg_proto_connect(mg_proto_t *p);
void mg_proto_disconnect(mg_proto_t *p);
void mg_proto_set_mtu(mg_proto_t *p, uint16_t mtu);
/* The subtitle max_words_bytes reported now: min(configured, ATT_MTU - 3 - 2). */
uint16_t mg_proto_subtitle_words_max(const mg_proto_t *p);
void mg_proto_subscribe(mg_proto_t *p, mg_ch_t ch, bool on);
void mg_proto_write(mg_proto_t *p, mg_ch_t ch, const uint8_t *data, size_t len);
/* Timeouts (the throughput test). Call about once a second. */
void mg_proto_tick(mg_proto_t *p);

/* The client's token proof matched on this connection. It gates mic output
 * (cfg.output_proof) and the device session's hello. */
bool mg_proto_proof_matched(const mg_proto_t *p);
/* The erase proof_clear_async started has ended: erased or not (the sealed clear's answer, in either session
 * kind). Under the gadget lock, as mgds_rx()/mgds_poll() are. */
void mg_proto_clear_done(mg_proto_t *p, bool erased);
/* The device session still owes a sealed clear's answer (mg_session_clear_owed). */
bool mg_proto_clear_owed(const mg_proto_t *p);
/* This connection may change the gadget (power off, the offline queue's erase,
 * a firmware update): not set up (no proof key), or its token proof matched.
 * The bench output switch doesn't count. */
bool mg_proto_owner_ok(const mg_proto_t *p);
/* Setup changed (a new setup committed, or a reset): a proof made with the old
 * key, and an exchange under way with it, no longer count, and the device
 * session (its hello included) opened with the old credential ends. */
void mg_proto_proof_reset(mg_proto_t *p);
/*
 * The latest matching proof's nonces on this connection, once (mgcommands.h,
 * Sealed credentials): they key one sealed device
 * session, so they're wiped as they're taken, and by a new challenge, a
 * reset or the connection's end. False before a match and once taken.
 */
bool mg_proto_proof_take_nonces(mg_proto_t *p, uint8_t client_nonce[MG_TOKEN_PROOF_NONCE_LEN],
                                uint8_t device_nonce[MG_TOKEN_PROOF_NONCE_LEN]);

/*
 * Mic output only to a proven central (cfg.output_proof).
 *
 * mg_proto_output_ok: output may go to the connected central now (no gate,
 * the bench switch on, or this connection's token proof matched). While it
 * may not, a capture (a press's, a client's start_mic, a Live call's talk
 * hold) is held whole in the capture's outbox (MG_CAPTURE_GUARD_S of its
 * format, the bench guard's size; without PSRAM what ops.capq_alloc gives),
 * nothing of it sent and the worker not woken for it; the match sends it all
 * in order (its change_data_type, the audio, its gestures and stop_mic queued
 * behind it). A hold that fills refuses more audio (mg_proto_send_data
 * false: the capture stops, the mic ring with it) and keeps what it has; a
 * capture doesn't start on a full hold. A failed confirm, a challenge with no
 * proof key, or a reset that leaves none discards the hold unsent and ends
 * the capture; the link's end discards it too. Held output not released
 * within MG_OUTPUT_STALE_MS of the capture's end is discarded (staleness,
 * checked at every pump and before any release). A client's
 * start_mic before the proof opens the mic only while a token-proof
 * challenge is outstanding on this connection; otherwise it's answered
 * proof_required. With no proof key at all no capture starts for the
 * central (mg_proto_live_begin false, start_mic and read_clip answered
 * proof_required): nothing can ever match. The bench switch lifts all of it.
 *
 * mg_proto_set_output_bench: the bench switch (the console's bench.out=1|0,
 * automation; RAM only, off at every boot, never stored), for a Mac bench
 * client that holds no proof key: on lifts the gate (a hold goes at once).
 */
bool mg_proto_output_ok(const mg_proto_t *p);
/* A capture is held for the proof now (something to send, the gate shut). */
bool mg_proto_output_held(const mg_proto_t *p);
/* Something waits on a clock: a held live turn's deadline, or a hold's staleness bound (the worker looks
 * every 20 ms while it does). */
bool mg_proto_output_waiting(const mg_proto_t *p);
/* The hold filled before the proof (the capture stopped; what's held is kept). */
bool mg_proto_output_full(const mg_proto_t *p);
void mg_proto_set_output_bench(mg_proto_t *p, bool on);
/* The switch from either console: the USB one's (kept across connections) or this connection's. */
bool mg_proto_output_bench(const mg_proto_t *p);
/* The Nordic UART's bench.out=1|0 (the bench override over Bluetooth): the same
 * switch for this connection only (ends at its disconnect; off at every boot and every connect). On while
 * connected lifts the gate as the USB switch does (a hold goes at once). */
void mg_proto_set_output_bench_conn(mg_proto_t *p, bool on);
bool mg_proto_output_bench_conn(const mg_proto_t *p);
/* What a central may learn of captures now: anything (no gate, the bench switch, or
 * this connection's proof matched); false: nothing about a held capture, neither its format nor that one
 * exists (request_status says the type from before it; mg_ble's status line, log mirror and link activity
 * leave it out). */
bool mg_proto_capture_visible(const mg_proto_t *p);
/* "open" (no gate), "bench", "proven" or "unproven", for the status line. */
const char *mg_proto_output_state(const mg_proto_t *p);
/* What a proof just did to a held capture (MG_OUT_NOTE_*) and its bytes: once. */
uint8_t mg_proto_take_output_note(mg_proto_t *p, uint32_t *bytes);
/* The gadget holds a proof key (it's been set up). */
bool mg_proto_has_key(const mg_proto_t *p);

/*
 * The held live turn (the switches in mg_config.h,
 * MG_HELD_TURN_*): a talk press while a client's link is up but the client
 * isn't proven or ready yet (the connect burst; route MG_ROUTE_HELD).
 *
 * mg_proto_held_turn_begin, just before the press's button_down (unix_ms: the
 * wall clock now, 0 if unset, for its clip): from here everything the turn
 * queues (button_down, change_data_type, its audio from the pre-roll on,
 * button_up, stop_mic) waits in the capture's outbox, whole (the guard's size;
 * a full hold refuses more audio and keeps what it has), and captures may
 * start before the client is subscribed. False (nothing held) without a link,
 * on a recovery mode, or with a held turn (or the bench guard's) open.
 *
 * Once the client is proven (or the bench switch is on) and ready (subscribed
 * to Control and Data, push-to-talk enabled), it goes live: everything held,
 * in order, as the link takes it (the pump checks at every call).
 *
 * mg_proto_held_turn_due: it's still held and MG_HELD_TURN_MS have passed
 * since it began, or the link dropped (MG_HELD_TURN_DROP_QUEUES; at the link's
 * end it's kept for this when the queue can take it, else discarded with the
 * rest). mg_proto_held_turn_queueable: the clip queue can take it now (cfg
 * queue, audio_queue_enabled, room for it and more, its format the clip's).
 * mg_proto_held_turn_take: its audio (bytes, its format) copied into out
 * (NULL: discarded), everything it queued removed unsent, the capture closed;
 * what was queued before it stays. The caller writes the clip.
 */
bool mg_proto_held_turn_begin(mg_proto_t *p, uint64_t unix_ms);
bool mg_proto_held_turn(const mg_proto_t *p);
bool mg_proto_held_turn_due(const mg_proto_t *p);
bool mg_proto_held_turn_queueable(const mg_proto_t *p);
uint32_t mg_proto_held_turn_bytes(const mg_proto_t *p);
size_t mg_proto_held_turn_take(mg_proto_t *p, uint8_t *out, size_t cap);
/* The voice task is capturing the held turn (it takes it at the deadline); off: the worker does. */
void mg_proto_held_turn_voice(mg_proto_t *p, bool on);
/* A capture is held whole now (the bench guard's turn, one held for the proof, or a held live
 * turn): its buffer is the guard's size (ops.capq_alloc reads it). */
bool mg_proto_capture_whole(const mg_proto_t *p);

/* ---- the audio side ---- */

/* Connected with notifications on: gestures and audio may go. */
/* (mg_proto_haptics is false on a board without a vibration motor.) */
bool mg_proto_ready(const mg_proto_t *p);

/* Where the client is (mg_link.h). set_up: the gadget holds a proof key (it's been set up). */
mg_link_t mg_proto_link(mg_proto_t *p, bool set_up);
bool mg_proto_ptt_enabled(const mg_proto_t *p);
/* A token proof answer still waiting for the stack (it goes first; busy after 1 s). */
#define MG_PROOF_ANSWER_MS 1000
bool mg_proto_proof_answer_pending(const mg_proto_t *p);
/* A new token proof's challenge just turned push-to-talk off: true once. */
bool mg_proto_take_ptt_cleared(mg_proto_t *p);
bool mg_proto_haptics(const mg_proto_t *p);
/* Codec for device-initiated capture: the last start_mic's, else the setting. */
uint8_t mg_proto_capture_codec(const mg_proto_t *p);
/* Largest Data payload per notification. */
size_t mg_proto_data_payload(const mg_proto_t *p);
/* The device's user changed a setting (speaker_volume from its buttons, say):
 * an unsolicited report in get_settings' answer shape, [get_settings, param,
 * value] (mgcommands.h, Settings reports). Nothing if the setting isn't
 * offered or no client is ready. speaker_volume's is owed until it leaves:
 * with no client ready,
 * the stack refusing or replies queued ahead of it, it goes at the first
 * pump that can send it (mg_proto_capture_pump, every connection's ready
 * included) with the level then, and not at all if that's the level the
 * client last saw; the client's own set is never reported back. */
void mg_proto_report_setting(mg_proto_t *p, uint8_t param);
/* An owed speaker_volume report goes now if it can (a connection's ready). */
void mg_proto_speaker_pump(mg_proto_t *p);
/* Setting client_app (19): the values this connection's client wrote, as
 * NUL-terminated UTF-8 ("" while unset). The caller holds the protocol's
 * lock; name / version may be NULL. (client_name, 18, is reserved: never
 * taken.) */
void mg_proto_client_app(const mg_proto_t *p, char *name, size_t name_n, char *version, size_t version_n);
/* n bytes are well-formed UTF-8 (no overlong forms, surrogates or code points
 * past U+10FFFF) with no control characters (C0, DEL, C1): what settings 18
 * and 19 accept, and what the status page shows of a central's GAP name. */
bool mg_proto_utf8_text_ok(const uint8_t *s, size_t n);
/* Charging started or stopped: an unsolicited report_battery_status to a
 * ready client. Nothing without ops.battery_status. */
void mg_proto_report_battery(mg_proto_t *p);
bool mg_proto_gesture(mg_proto_t *p, uint8_t gesture);
/* A press the device detected, from src (mg_press_src.h). A physical one opens start_mic for MG_PRESS_WINDOW_MS;
 * any other source does nothing. */
void mg_proto_press(mg_proto_t *p, uint8_t src);
/* A press of the device's own within MG_PRESS_WINDOW_MS. */
bool mg_proto_press_window(const mg_proto_t *p);
/* set_unix_time's floor (mgcommands.h): 2024-01-01T00:00:00Z; earlier is refused (invalid_value). */
#define MG_CLOCK_FLOOR_MS 1704067200000LL
/* Its ceiling: 9999-12-31T23:59:59.999Z; later is refused (invalid_value). */
#define MG_CLOCK_CEIL_MS 253402300799999LL
/* Stream audio past the credit the device granted, dropped and counted (writes, bytes); the same for video. */
void mg_proto_credit_overruns(const mg_proto_t *p, uint32_t *writes, uint32_t *bytes);
void mg_proto_video_credit_overruns(const mg_proto_t *p, uint32_t *writes, uint32_t *bytes);
/* Announces the codec (ending any clip download) and opens the utterance. */
bool mg_proto_live_begin(mg_proto_t *p, uint8_t codec);
/* The same at a format (mg_proto_turn_format, or a start_mic's): its label says it. */
bool mg_proto_live_begin_fmt(mg_proto_t *p, const mg_audio_format_t *fmt);
/* The format of the user's push-to-talk: LC3 in a call else
 * setting 7, at capture_sample_rate where that codec takes it (else 16 kHz),
 * at capture_bitrate_kbps clamped into its range. */
void mg_proto_turn_format(const mg_proto_t *p, mg_audio_format_t *out);
/* A start_mic's format from its parameters (codec, rate, kb/s; 0: omitted):
 * the codec if supported else setting 7, the rate if listed for it else
 * 16 kHz, the bitrate clamped (never refused). */
void mg_proto_mic_format(const mg_proto_t *p, uint8_t codec, uint16_t rate, uint16_t kbps, mg_audio_format_t *out);
bool mg_proto_live(const mg_proto_t *p);
/* Queues whole frames of the open utterance (mg_capq_t) and sends what the
 * link takes now; false if no utterance is open, or the proof hold
 * is full: the capture stops there. Never blocks: what doesn't go now is
 * held, the oldest audio dropped only when the buffer is full and the
 * central is proven. */
bool mg_proto_send_data(mg_proto_t *p, const uint8_t *data, size_t len);
/* Closes the utterance; notify_stop queues [stop_mic], which marks the end of
 * every capture: sent after the audio held before it, never dropped. */
void mg_proto_live_end(mg_proto_t *p, bool notify_stop);
/* A capture not kept (the mute switch, a press too short) that's still held for this connection's token proof:
 * discarded whole, never sent and never queued as a clip, with no stop_mic (nothing of it went). False, and
 * nothing done, when output may go (the proof matched, the bench switch, no gate): mg_proto_live_end then ends
 * it after what went. */
bool mg_proto_live_discard(mg_proto_t *p);
/*
 * The bench guard (console word ptt.minrise=N, automation; off at every
 * boot, never stored; added after five unheard turns were
 * transcribed as "Help."): a push-to-talk turn the gadget starts is held on
 * the gadget until its capture rises N dB over its own floor, so a client
 * never receives a turn the mic didn't hear.
 *
 * mg_proto_turn_hold, just before the turn's button_down: from here
 * everything the turn queues (button_down, change_data_type, its audio from
 * the pre-roll on, button_up, stop_mic) waits in the capture's outbox and
 * nothing of it goes (what an earlier utterance still holds drains as
 * usual); the outbox holds the whole turn (MG_CAPTURE_GUARD_S of its format,
 * grown if an earlier utterance's buffer is smaller); request_status keeps
 * reporting the data type from before the turn; the turn's claim of the
 * speaker (mg_proto_claim_turn) waits too, holding a reply instead of
 * dropping or flushing it. False (nothing held) when N is 0, the client
 * isn't ready, or a held turn is already open.
 *
 * mg_proto_turn_rise, with the capture's level so far (mg_capture_meter_t,
 * the utterance summary's own meter): the rise is the loudest 1 s window
 * (loud1s; the whole capture while it's under a second; rms where no ring)
 * over the floor (the quietest 20 ms window since the turn's first sample,
 * pre-roll included), in dB. At N or more the turn is released: the claim
 * happens now (stop_streaming drop, or the call's flush), and everything
 * held goes in order as the link takes it (button_down, change_data_type,
 * the held audio as a burst in whole frames per notification, then the rest
 * as it comes): the pump's usual pacing. True when this call released it.
 *
 * mg_proto_turn_drop, at the end of a turn still held: everything it queued
 * is removed unsent (no gesture, change_data_type, Data or stop_mic: the
 * client never learns of it), the capture closes (live off), the data type
 * goes back, and nothing_heard counts it (ptt_nothing). The claim stays
 * deferred: its off (mg_proto_claim_turn(false)) lets a reply held since the
 * touch-down play on from where it paused, as after a tap. False if no turn
 * was held (never, or the link dropped and took it).
 */
bool mg_proto_turn_hold(mg_proto_t *p, uint8_t min_rise_db);
bool mg_proto_turn_held(const mg_proto_t *p);
bool mg_proto_turn_rise(mg_proto_t *p, const mg_capture_level_t *lv);
bool mg_proto_turn_drop(mg_proto_t *p, bool nothing_heard);
/* Sends what the capture's outbox holds, as far as the link takes it now,
 * then the replies and the commands waiting for them (mg_cmdq_t). True while
 * something is still held (call it again: every 5 ms). The worker task's
 * alone: the commands that waited run only here, never on the voice task's
 * audio path (its stack can be in PSRAM, and a command can write flash). */
bool mg_proto_capture_pump(mg_proto_t *p);
/* Something of a capture (audio, a gesture, its end) is still to go. */
bool mg_proto_capture_pending(const mg_proto_t *p);
/* Audio held now, in bytes. */
uint32_t mg_proto_capture_held(const mg_proto_t *p);
/* The level of the capture whose end (stop_mic) is queued next: call it
 * before mg_proto_live_end(). Its summary carries it once that end has gone. */
void mg_proto_capture_level(mg_proto_t *p, const mg_capture_level_t *lv);
/* The last delivered end's summary, for mg.voice: "utterance over BLE: 3.4 s,
 * 27200 bytes, 0 frames dropped, rms -24.1 dBFS, peak -3.2 dBFS, floor
 * -61.0 dBFS, loud1s -19.8 dBFS, 0 clipped; end sent 1900 ms after the
 * capture ended, up to 2950 ms held for the link" (loud1s: the loudest 1 s
 * window, where measured). */
int mg_proto_format_utterance(const mg_proto_t *p, char *out, size_t cap);

/* Control's queue counts for the console's status line (mg_reply_stats_t),
 * and as text: "dropped=0 held_mtu=0 ..." (json: an object of the same). */
void mg_proto_reply_stats(const mg_proto_t *p, mg_reply_stats_t *out);
int mg_reply_stats_format(const mg_reply_stats_t *s, bool json, char *out, size_t cap);

/* ---- playback (mg_command_stream_audio) ---- */

/*
 * The device's own sound (a chirp, a bench test, a saved note) takes the
 * speaker: a stream in progress ends with stop_streaming drop, and
 * start_streaming is refused as busy until it's released. In a Live call
 * (mgcommands.h: the device ends a call's stream only when it
 * can't go on) the call's reply stream is paused
 * instead and resumes where it was when the sound is over: nothing it holds
 * is flushed or lost, Data and credits go on as usual (the call's depth cap
 * bounds what piles up), and a start_streaming meanwhile is taken and waits
 * the same way (mg_proto_play_held). No ESP32 board mixes two sources into
 * its output path (the reply's player and the voice loop each write the
 * speaker), so the reply pauses rather than ducks under the sound.
 */
void mg_proto_claim_audio(mg_proto_t *p, bool claim);
/* The playback task holds the stream (it plays nothing and pops nothing): a
 * push-to-talk turn, the device's own sound pausing a Live call's reply, or
 * a talk press's touch-down (mg_proto_barge_pause). */
bool mg_proto_play_held(const mg_proto_t *p);
/*
 * The barge-in echo rule: a gadget with
 * pre-roll and a speaker mutes its reply at a talk press's touch-down, so
 * the pre-roll it records before the hold threshold carries none of the
 * Muse's own words. on: the playback task holds whatever plays (a reply, a
 * Live call's stream, one that starts meanwhile) exactly as for a turn:
 * nothing popped or played, the ring and the task's decoded FIFO kept, Data
 * and credits (buffer_update) going on, no underrun counted (the stream's
 * clock stops with it), and the client's stop_streaming or flush still
 * applies. off (the press was a tap, released before the hold threshold):
 * playback resumes from the very sample it paused at. At the threshold the
 * turn is claimed as before (mg_proto_claim_turn: stop_streaming drop
 * outside a call, the call's flush in one), which ends the pause. A pause
 * nobody ends lapses after MG_PROTO_BARGE_PAUSE_MAX_MS (a press the voice
 * task never decided: the reply is never left stuck).
 */
#define MG_PROTO_BARGE_PAUSE_MAX_MS 1500
void mg_proto_barge_pause(mg_proto_t *p, bool on);
/* The pause above holds playback now (on, and not lapsed). */
bool mg_proto_barge_held(const mg_proto_t *p);
/* A push-to-talk turn (mgcommands.h, Audio playback): on, a stream already
 * playing ends (stop_streaming drop) and the speaker waits (a held turn's
 * drop or flush waits for its release, mg_proto_turn_hold: until then the
 * reply is held as for a turn, and a dropped turn's off resumes it); a start_streaming
 * during the turn is accepted and buffered under credits, and plays once the
 * turn (with its tail) is over and this goes off. A Live call's one reply
 * stream (keep_open) is flushed instead, as mg_stream_audio_flush does, and
 * a buffer_update sent: it stays open, and the next reply on it waits for
 * the turn the same way. */
void mg_proto_claim_turn(mg_proto_t *p, bool on);
/*
 * Sends the periodic buffer_update while streaming. Call every 5 ms. While a
 * live depth is honored or a start before active is pending (mg_play_t
 * live_us or slow_link: a
 * client paused at a capped depth waits on fresh credit, and a pending
 * notification keeps the peripheral listening at every connection event
 * despite its slave latency) it goes every 10 ms of playout while the credit
 * changes, once the previous one has left (mg_proto_control_sent, or
 * 40 ms), so at most one is in flight per connection event. On the
 * whole-buffer path with the link active it goes on change only: once
 * MG_PLAY_UPDATE_FREED of room was freed, or MG_PLAY_UPDATE_CHANGE_MS on with
 * any change, and never while any Control notification is in the stack
 * (video credit takes the slot first: call mg_proto_video_tick before this).
 * At least every 250 ms either way.
 */
void mg_proto_stream_tick(mg_proto_t *p);
/* The whole-buffer path's on-change rule (no live depth, link active): an update once
 * this much room was freed since the last, or this long on with any change. */
#define MG_PLAY_UPDATE_FREED 1024
#define MG_PLAY_UPDATE_CHANGE_MS 50
/* The stack sent a Control notification (NimBLE's NOTIFY_TX on Control). */
void mg_proto_control_sent(mg_proto_t *p);
/* The playback task found the stream DONE and has played it out: notify stop_streaming and go idle. */
void mg_proto_stream_finished(mg_proto_t *p);

/* ---- video (mg_command_stream_video) ---- */

/* Data bytes dropped since init: their type (change_data_type) had no open stream. */
uint32_t mg_proto_rx_discarded(const mg_proto_t *p);
bool mg_proto_video_active(const mg_proto_t *p);
/* The device's user wants a stream (true) or no longer (false): request_streaming. */
void mg_proto_video_request(mg_proto_t *p, bool want);
/* The device can't go on (or decode) until a keyframe: request_keyframe. */
void mg_proto_video_request_keyframe(mg_proto_t *p);
/* Ends the stream from the device's side: stop_streaming drop to the client. */
void mg_proto_video_stop(mg_proto_t *p);
/* Numbers for the next buffer_update. */
void mg_proto_video_stats(mg_proto_t *p, uint16_t shown, uint16_t dropped, uint8_t decode_ms);
/* The last picture the display finished drawing: its frame number, at now_ms (buffer_update's echo). */
void mg_proto_video_shown(mg_proto_t *p, uint16_t number, uint32_t at_ms);
/* The content (mg_video_content_t) of the stream that's on. */
uint8_t mg_proto_video_content(const mg_proto_t *p);
/* Video credit: buffer_update once 1 KB has been taken
 * from the ring or 1 KB received (MG_VIDEO_CREDIT_STEP), else 20 ms after any
 * news (bytes in, room freed, a picture shown; MG_VIDEO_NEWS_MS), once the
 * previous Control notification has left (mg_proto_control_sent, or 40 ms):
 * at most one in flight per connection event. A camera stream also gets one
 * every 250 ms with no news (MG_VIDEO_KEEPALIVE_MS); a UI stream none while
 * its picture is still. request_buffer_update is answered at once. Call every
 * 5 ms while a stream is on, and when the decoder frees room. */
#define MG_VIDEO_CREDIT_STEP 1024
#define MG_VIDEO_NEWS_MS 20
#define MG_VIDEO_KEEPALIVE_MS 250
void mg_proto_video_tick(mg_proto_t *p);

/* ---- live call (mg_command_live_call) ---- */

/*
 * The device's user starts (true) or ends (false) a live call. A start no
 * client has answered (with connecting, live, ended or failed) stays pending
 * and goes again to each client that becomes ready (request_status).
 */
void mg_proto_live_call_request(mg_proto_t *p, bool start);
/* The call's state as the client last reported it (idle with no client). */
uint8_t mg_proto_live_call_state(const mg_proto_t *p);
/* Why the last failed state came (mg_live_call_failure_reason_t): 0 when the client said nothing. */
uint8_t mg_proto_live_call_failure_reason(const mg_proto_t *p);
/* The last live_call state came from a central without this connection's token proof, and wasn't the answer to
 * its own pending start: the UI shows it only on an unlocked, awake gadget, never wakes
 * the screen for it, and never lets it hold off the passcode lock. */
bool mg_proto_live_call_unproven(const mg_proto_t *p);
/* The UI holds the phone's call behind the passcode keypad: until it's released (the unlock), the call's
 * reply audio is refused busy. Ends with the call. */
void mg_proto_live_call_held(mg_proto_t *p, bool held);
bool mg_proto_live_call_pending(const mg_proto_t *p);
/* The codec of the user's push-to-talk: LC3 in a call, else setting 7. */
uint8_t mg_proto_turn_codec(const mg_proto_t *p);

/* ---- UI input (mg_command_ui_input) ---- */

/* The client asked for touch events on this connection. */
bool mg_proto_touch_enabled(const mg_proto_t *p);
/* Notifies a touch event (if the client asked for them): time_ms the device's
 * clock, id the pointer. False if it wasn't sent. */
bool mg_proto_touch(mg_proto_t *p, const mg_touch_event_t *ev, uint32_t time_ms, uint8_t id);
/* The HID's mode in use and status flags: hid_status to the client when they change. */
void mg_proto_hid_status(mg_proto_t *p, uint8_t mode, uint8_t flags);
/* A UI control (mg_ui_control_t) now, as the client said or the user set it. */
uint8_t mg_proto_ui_control(const mg_proto_t *p, uint8_t id);
/* The device's user set a UI control: kept, and sent to the client. */
bool mg_proto_ui_control_set(mg_proto_t *p, uint8_t id, uint8_t value);

/* ---- device session (mg_command_session) ---- */

/* The device session is offered (cfg.session and its ops). */
bool mg_proto_session_offered(const mg_proto_t *p);
/* The client reports the VM session open. */
bool mg_proto_session_open(const mg_proto_t *p);
/* This connection's device session is sealed (its hello asked for sealed
 * credentials): its data is sealed too, both ways. */
bool mg_proto_session_sealed(const mg_proto_t *p);
/* The open session's id (mg_session_id()), 0 if none. */
uint32_t mg_proto_session_id(const mg_proto_t *p);
/* mg_session_reask_marks(): a change restarts the re-ask series (0s without the session). */
void mg_proto_session_reask_marks(const mg_proto_t *p, uint32_t *hellos, uint32_t *saved);
/* A message to the VM (a ServiceRequestEnvelope) in session (its id),
 * copied; false if that session isn't the open one or the message can't be
 * queued (mg_session_send()). */
bool mg_proto_session_send(mg_proto_t *p, uint32_t session, const uint8_t *msg, size_t len);
/* The open session's nudge (mg_session_nudge); nothing without one. */
void mg_proto_session_nudge(mg_proto_t *p);
/* Bytes mg_proto_session_send() takes now, and the largest message the client takes. */
size_t mg_proto_session_room(const mg_proto_t *p);
uint32_t mg_proto_session_client_max(const mg_proto_t *p);
/* The oldest whole message from the VM (a ServiceResponseEnvelope) in
 * session, copied out; its credit goes back. 0 if none, or session isn't
 * the open one. */
size_t mg_proto_session_take(mg_proto_t *p, uint32_t session, uint8_t *out, size_t cap);
/* open_request: ask the client to open or close the session. */
void mg_proto_session_request(mg_proto_t *p, bool open);
/* Sends what the session has pending. True if something is still pending. */
bool mg_proto_session_pump(mg_proto_t *p);
/* mg_session_take_update() / mg_session_update_saved() on the VM session
 * (false / nothing without one): the store between them runs outside the
 * protocol lock. */
bool mg_proto_session_take_update(mg_proto_t *p, char **access, char **refresh, uint64_t *expires, uint32_t *gen);
void mg_proto_session_update_saved(mg_proto_t *p, uint32_t gen, bool saved);
/* Stores a credentials_update's pair that waits (mg_session_config_t.store_later). */
bool mg_proto_session_commit(mg_proto_t *p);

/* Ends a read_clip download and clears the clip queue (audio_queue clear,
 * and an ownership change: a reset or a new setup). False if the clear failed. */
bool mg_proto_queue_forget(mg_proto_t *p);
/* A read_clip download: sends the next chunk. True while there's more. */
bool mg_proto_transfer_active(const mg_proto_t *p);
bool mg_proto_pump(mg_proto_t *p);

/* A haptic_play status: [device_action, haptic_play, id, status, reason]. */
void mg_proto_haptic_status(mg_proto_t *p, uint8_t id, uint8_t status, uint8_t reason);
