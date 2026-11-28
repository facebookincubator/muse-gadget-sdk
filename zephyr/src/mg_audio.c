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

#include "mg_audio.h"
#include "mg_cap_level.h"
#include "mg_voice.h"
#include "mg_mic_gain.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "mg_clock.h"
#include "mg_connparam.h"
#include "mg_core.h"
#include "mg_health.h"
#include "mg_hpf.h"
#include "mg_out_hold.h"
#include "mg_led.h"
#include "mg_session.h"
#include "mg_settings.h"
#include "mg_token_proof.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_AUDIO_QUEUE)
#include "mg_queue.h"
#endif

LOG_MODULE_REGISTER(mg_audio, CONFIG_MG_LOG_LEVEL);

#define STACK_SIZE 6144
#define MAX_CHUNK  244

struct request {
	enum mg_audio_activity want;
	uint8_t codec;
	uint16_t rate;
	uint16_t kbps;
	bool ptt;
	bool held; /* pre-roll: hold everything back until committed */
	uint8_t gate; /* enum mg_audio_gate */
	uint32_t deadline; /* a held turn's: to the clip queue then (uptime ms) */
	uint16_t clip;
	uint32_t gen;
};

/* Pre-roll backlog: at most 1 s of encoded frames (SBC 7.5 KB/s, LC3 at the
 * default 40-byte frames 4 KB/s). */
#define BACKLOG_BYTES 7680
static uint8_t backlog[BACKLOG_BYTES];
/* For the request with generation commit_gen: 1 commit, 2 discard. */
static atomic_t commit_state;
static atomic_t commit_gen;

static K_MUTEX_DEFINE(lock);
static K_SEM_DEFINE(wake, 0, 1);
static struct request req;
static struct mg_audio_format cur_fmt;
static atomic_t gain_q10;
static atomic_t gain_level = CONFIG_MG_MIC_DEFAULT_GAIN;
/* The last turns' capture levels, newest first. */
#define CAP_RECORDS 4
static struct mg_cap_record cap_rec[CAP_RECORDS];
static size_t cap_n;
static struct k_spinlock cap_lock;

size_t mg_audio_levels(struct mg_cap_record *out, size_t max)
{
	k_spinlock_key_t key = k_spin_lock(&cap_lock);
	size_t n = MIN(max, cap_n);

	memcpy(out, cap_rec, n * sizeof(*out));
	k_spin_unlock(&cap_lock, key);
	return n;
}

static void cap_store(const struct mg_cap_record *r)
{
	k_spinlock_key_t key = k_spin_lock(&cap_lock);

	memmove(&cap_rec[1], &cap_rec[0], (CAP_RECORDS - 1) * sizeof(cap_rec[0]));
	cap_rec[0] = *r;
	cap_n = MIN(cap_n + 1, (size_t)CAP_RECORDS);
	k_spin_unlock(&cap_lock, key);
}
/* The tuned gain at CONFIG_MG_MIC_DEFAULT_GAIN (Kconfig.mg). */
#define MG_MIC_TUNED_Q10 CONFIG_MG_MIC_TUNED_Q10

static int16_t block[MG_AUDIO_MAX_BLOCK_SAMPLES];
static int16_t acc[MG_AUDIO_MAX_BLOCK_SAMPLES + MG_CODEC_MAX_FRAME_SAMPLES];
static uint8_t frame[MG_CODEC_MAX_FRAME_BYTES];
static uint8_t chunk[MAX_CHUNK];

static struct k_work idle_work;

/* ---- A gated capture's output, held until the gate opens ---- */

static K_MUTEX_DEFINE(out_lock);
static struct mg_out_hold hold;
static uint8_t gate;            /* armed: output goes into the hold */
static bool turn_clipped;       /* the held turn became a clip: its gestures dropped */
static uint32_t cap_end;        /* the gated capture's end (uptime ms; 0: capturing) */
static uint32_t hold_gen;       /* the gated request's generation (0: not submitted yet) */
static uint32_t turn_deadline;  /* a held turn's deadline (uptime ms) */
static bool turn_noqueue;       /* a held turn the clip queue couldn't take: held on */
/* Kept free in the hold for the capture's last Control messages (button_up,
 * stop_mic), so a full hold still ends its turn properly. */
#define HOLD_CTRL_RESERVE 64
static struct mg_audio_format held_fmt; /* request_status's type once released */
static atomic_t gate_events;
/* No capture without its light: the light must be
 * driven on within this long of the capture's start, or nothing is captured
 * (counted). */
#define MG_AUDIO_LIGHT_WAIT_MS 200
static atomic_t light_refused;
#define GATE_EV_FAIL BIT(0)
#define GATE_EV_DROP BIT(1)
static struct mg_audio_gate_stats gstats;
static atomic_t voice_seen; /* MG_AUDIO_GATE_VOICE's turn rose above room noise */
static atomic_t last_silent; /* the last talk turn was dropped as silent */

bool mg_audio_last_turn_silent(void)
{
	return atomic_get(&last_silent) != 0;
}

static bool releasing; /* out_release() has started on this hold */

/* Under out_lock. */
static void out_disarm(void)
{
	releasing = false;
	free(hold.buf);
	mg_out_hold_init(&hold, NULL, 0);
	gate = MG_AUDIO_GATE_NONE;
}

static int out_arm(uint8_t g)
{
	k_mutex_lock(&out_lock, K_FOREVER);
	if (gate != MG_AUDIO_GATE_NONE) {
		k_mutex_unlock(&out_lock);
		return -EBUSY;
	}
	uint8_t *b = malloc(MG_AUDIO_HOLD_BYTES);

	if (b == NULL) {
		k_mutex_unlock(&out_lock);
		return -ENOMEM;
	}
	mg_out_hold_init(&hold, b, MG_AUDIO_HOLD_BYTES);
	gate = g;
	hold_gen = 0;
	turn_clipped = false;
	turn_noqueue = false;
	cap_end = 0;
	turn_deadline = k_uptime_get_32() + MG_AUDIO_HELD_TURN_MS;
	atomic_clear(&gate_events);
	gstats.held++;
	if (g == MG_AUDIO_GATE_TURN) {
		gstats.turns++;
	}
	k_mutex_unlock(&out_lock);
	return 0;
}

bool mg_audio_output_held(void)
{
	k_mutex_lock(&out_lock, K_FOREVER);
	bool held = gate != MG_AUDIO_GATE_NONE;

	k_mutex_unlock(&out_lock);
	return held;
}

void mg_audio_gate_stats(struct mg_audio_gate_stats *st)
{
	k_mutex_lock(&out_lock, K_FOREVER);
	*st = gstats;
	k_mutex_unlock(&out_lock);
}

void mg_audio_gate_fail(void)
{
	atomic_or(&gate_events, GATE_EV_FAIL);
}

void mg_audio_gate_drop(void)
{
	atomic_or(&gate_events, GATE_EV_DROP);
}

/* 1: held (or dropped); 0: send it now. -ENOSPC: the hold is full. */
static int out_hold_or_send(uint8_t kind, const uint8_t *p, size_t n)
{
	int r = 0;

	k_mutex_lock(&out_lock, K_FOREVER);
	if (gate != MG_AUDIO_GATE_NONE) {
		if (kind == MG_OUT_DATA && hold.len + 3 + n + HOLD_CTRL_RESERVE > hold.cap) {
			gstats.full++;
			r = -ENOSPC;
		} else if (mg_out_hold_push(&hold, kind, p, n)) {
			r = 1;
		} else {
			gstats.full++;
			r = -ENOSPC;
		}
	} else if (turn_clipped && kind == MG_OUT_CTRL) {
		r = 1; /* the turn is a clip now: its gestures and stop_mic stay home */
	}
	k_mutex_unlock(&out_lock);
	return r;
}

void mg_audio_out_control(const uint8_t *buf, size_t len)
{
	if (out_hold_or_send(MG_OUT_CTRL, buf, len) == 0) {
		(void)mg_session_send_control(buf, len);
	}
}

static int out_ctrl(const uint8_t *buf, size_t len)
{
	int r = out_hold_or_send(MG_OUT_CTRL, buf, len);

	if (r == 0) {
		(void)mg_session_send_control(buf, len);
	}
	return r < 0 ? r : 0;
}

/* 0 sent or held; -ENOSPC: the hold is full; else the send's error. */
static int out_data(const uint8_t *buf, size_t len, k_timeout_t wait)
{
	int r = out_hold_or_send(MG_OUT_DATA, buf, len);

	if (r == 0) {
		/* Checked at the send too: a proof reset (or the bench switch
		 * going off) ends the capture from the work queue, and what is
		 * encoded meanwhile must not go. */
		if (!mg_core_output_ok()) {
			return -EACCES;
		}
		return mg_session_send_data(buf, len, wait);
	}
	return r < 0 ? r : 0;
}

static void out_discard(const char *why, uint32_t *count);

enum release {
	RELEASE_DONE,      /* all of it went: output flows directly now */
	RELEASE_MORE,      /* the queue is full: the rest goes at the next pump */
	RELEASE_DISCARDED, /* the gate's connection or proof ended: discarded */
};

/* The gate opened: everything held goes, in order, then output flows
 * directly (the gate is cleared only with the hold empty, so the core's
 * gestures meanwhile queue behind it). Never waits for the notification
 * queue (it runs on the capture thread, which must keep reading the mic):
 * what doesn't fit stays held for the next pump. Only to the connection and
 * the proof it opened for: if either ends meanwhile the rest is discarded
 * unsent. */
static enum release out_release(uint8_t g)
{
	static uint8_t rec[MAX_CHUNK + 16];
	static uint32_t gen, epoch, bytes;

	if (!releasing) {
		releasing = true;
		gen = mg_transport_conn_gen();
		epoch = mg_token_proof_epoch();
		bytes = 0;
	}
	for (;;) {
		uint8_t kind;
		const uint8_t *p;
		size_t n;
		int err;

		if (!mg_core_gate_open(g) || gen != mg_transport_conn_gen() ||
		    epoch != mg_token_proof_epoch()) {
			out_discard("output no longer allowed while held output went",
				    &gstats.discards);
			return RELEASE_DISCARDED;
		}
		k_mutex_lock(&out_lock, K_FOREVER);
		if (!mg_out_hold_peek(&hold, &kind, &p, &n)) {
			out_disarm();
			gstats.releases++;
			k_mutex_unlock(&out_lock);
			break;
		}
		n = MIN(n, sizeof(rec));
		memcpy(rec, p, n);
		k_mutex_unlock(&out_lock);
		if (kind == MG_OUT_CTRL) {
			err = mg_session_try_control(rec, n);
		} else {
			err = mg_session_send_data(rec, n, K_NO_WAIT);
		}
		if (err == -ENOMEM || err == -EBUSY) {
			return RELEASE_MORE; /* held output is never dropped */
		}
		if (kind == MG_OUT_CTRL && n >= 1 && rec[0] == mg_command_change_data_type) {
			k_mutex_lock(&lock, K_FOREVER);
			cur_fmt = held_fmt; /* visible from now on */
			k_mutex_unlock(&lock);
		}
		if (kind == MG_OUT_DATA) {
			bytes += n;
		}
		k_mutex_lock(&out_lock, K_FOREVER);
		mg_out_hold_pop(&hold);
		k_mutex_unlock(&out_lock);
	}
	if (g == MG_AUDIO_GATE_VOICE) {
		LOG_INF("talk: voice above room noise: %u bytes held went first", bytes);
	} else {
		LOG_INF("mg.ble: capture held for the token proof: proof matched, %u bytes held "
			"went first",
			bytes);
	}
	return RELEASE_DONE;
}

static void out_discard(const char *why, uint32_t *count)
{
	k_mutex_lock(&out_lock, K_FOREVER);
	uint32_t bytes = hold.data_bytes;

	if (gate != MG_AUDIO_GATE_NONE) {
		out_disarm();
		(*count)++;
	}
	k_mutex_unlock(&out_lock);
	LOG_INF("mg.ble: %s: %u bytes held discarded unsent", why, bytes);
}

enum pump {
	PUMP_NONE,      /* nothing held, released, or still waiting */
	PUMP_DISCARD,   /* discarded: stop capturing, send nothing more */
	PUMP_CLIP,      /* a held turn's deadline: to the clip queue now */
	PUMP_CLIP_DROP, /* a held turn and the link ended: to the clip queue, else discard */
};

/* While a capture's output is held: release it once the gate opens, or
 * discard it (a failed proof, the link's end for anything but a held turn,
 * staleness), or say a held turn goes to the clip queue (its deadline, or
 * the link's end). */
static enum pump out_pump(void)
{
	atomic_val_t ev = atomic_clear(&gate_events);

	k_mutex_lock(&out_lock, K_FOREVER);
	uint8_t g = gate;
	uint32_t end = cap_end, dl = turn_deadline;

	k_mutex_unlock(&out_lock);
	if (g == MG_AUDIO_GATE_NONE) {
		return PUMP_NONE;
	}
	uint32_t now = k_uptime_get_32();

	if (ev & GATE_EV_FAIL) {
		out_discard("no valid proof", &gstats.discards);
		return PUMP_DISCARD;
	}
	if (ev & GATE_EV_DROP) {
		if (g == MG_AUDIO_GATE_TURN) {
			return PUMP_CLIP_DROP;
		}
		out_discard("the link ended before the proof", &gstats.discards);
		return PUMP_DISCARD;
	}
	if (g == MG_AUDIO_GATE_VOICE ? atomic_get(&voice_seen) != 0 : mg_core_gate_open(g)) {
		return out_release(g) == RELEASE_DISCARDED ? PUMP_DISCARD : PUMP_NONE;
	}
	if (g == MG_AUDIO_GATE_TURN && !turn_noqueue && (int32_t)(now - dl) >= 0) {
		return PUMP_CLIP;
	}
	if (mg_out_hold_stale(now, end)) {
		out_discard("held output past its 3 s", &gstats.stale);
		return PUMP_DISCARD;
	}
	return PUMP_NONE;
}

#if defined(CONFIG_MG_AUDIO_QUEUE)
static uint32_t clip_unix_s(uint32_t back_ms);

/* A held turn to the clip queue: its audio so far opens a
 * clip, back-dated to the press; its gestures and change_data_type stay
 * home. false (still held) without a queue to take it. */
static bool held_to_clip(const struct mg_audio_format *fmt, uint8_t codec, uint32_t *stored)
{
	if (!mg_settings_get()->audio_queue_enabled || !mg_core_clips_ready()) {
		turn_noqueue = true; /* held on, under the staleness bound */
		return false;
	}
	k_mutex_lock(&out_lock, K_FOREVER);
	uint32_t back = hold.data_bytes / fmt->frame_bytes * fmt->frame_us / 1000;
	int err = mg_queue_begin_fmt(codec, fmt->sample_rate, fmt->channels, fmt->frame_bytes,
				     fmt->frame_us, k_uptime_get_32() - back, clip_unix_s(back));

	if (err) {
		turn_noqueue = true;
		k_mutex_unlock(&out_lock);
		LOG_WRN("held live turn: the clip queue can't take it (%d)", err);
		return false;
	}
	uint8_t kind;
	const uint8_t *p;
	size_t n;

	while (mg_out_hold_peek(&hold, &kind, &p, &n)) {
		if (kind == MG_OUT_DATA && mg_queue_append(p, n) == 0) {
			*stored += n;
		}
		mg_out_hold_pop(&hold);
	}
	out_disarm();
	turn_clipped = true;
	gstats.queued++;
	k_mutex_unlock(&out_lock);
	LOG_INF("held live turn: not proven and ready within %d ms (or the link ended): %u ms "
		"to the clip queue, the turn goes on into it",
		MG_AUDIO_HELD_TURN_MS, back);
	return true;
}
#endif

static void idle_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	mg_core_audio_idle();
}

static uint32_t submit_gated(enum mg_audio_activity want, uint8_t codec, uint16_t rate,
			     uint16_t kbps, bool ptt, uint16_t clip, bool held, uint8_t g)
{
	uint32_t gen;

	if (want == MG_AUDIO_LIVE || want == MG_AUDIO_RECORDING) {
		/* A new capture: a clipped held turn's gestures are behind us. */
		k_mutex_lock(&out_lock, K_FOREVER);
		if (g == MG_AUDIO_GATE_NONE) {
			turn_clipped = false;
		}
		k_mutex_unlock(&out_lock);
	}
	k_mutex_lock(&lock, K_FOREVER);
	req.gate = g;
	if (g != MG_AUDIO_GATE_NONE) {
		hold_gen = req.gen + 1; /* this request's, below (read under out_lock too) */
	}
	req.want = want;
	req.codec = codec;
	req.rate = rate ? rate : MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ;
	req.kbps = kbps ? kbps : MG_CAPTURE_DEFAULT_BITRATE_KBPS;
	req.ptt = ptt;
	req.held = held;
	req.clip = clip;
	gen = ++req.gen;
	if (held) {
		atomic_set(&commit_state, 0);
		atomic_set(&commit_gen, (atomic_val_t)gen);
	}
	k_mutex_unlock(&lock);
	k_sem_give(&wake);
	return gen;
}

static uint32_t submit_held(enum mg_audio_activity want, uint8_t codec, uint16_t rate,
			    uint16_t kbps, bool ptt, uint16_t clip, bool held)
{
	return submit_gated(want, codec, rate, kbps, ptt, clip, held, MG_AUDIO_GATE_NONE);
}

static uint32_t submit(enum mg_audio_activity want, uint8_t codec, uint16_t rate, uint16_t kbps,
		       bool ptt, uint16_t clip)
{
	return submit_held(want, codec, rate, kbps, ptt, clip, false);
}

static bool current(uint32_t gen)
{
	k_mutex_lock(&lock, K_FOREVER);
	bool same = req.gen == gen;

	k_mutex_unlock(&lock);
	return same;
}

int mg_audio_start_live(uint8_t codec, uint16_t rate, uint16_t kbps, bool ptt)
{
	struct mg_audio_format fmt;

	if (mg_codec_format_cfg(codec, rate, kbps, &fmt)) {
		return -ENOTSUP;
	}
	submit(MG_AUDIO_LIVE, codec, rate, kbps, ptt, 0);
	return 0;
}

int mg_audio_start_live_gated(uint8_t codec, uint16_t rate, uint16_t kbps)
{
	struct mg_audio_format fmt;
	int err;

	if (mg_codec_format_cfg(codec, rate, kbps, &fmt)) {
		return -ENOTSUP;
	}
	err = out_arm(MG_AUDIO_GATE_PROOF);
	if (err) {
		return err;
	}
	submit_gated(MG_AUDIO_LIVE, codec, rate, kbps, false, 0, false, MG_AUDIO_GATE_PROOF);
	return 0;
}

int mg_audio_start_turn_held(void)
{
	int err = out_arm(MG_AUDIO_GATE_TURN);

	if (err) {
		return err;
	}
	/* The clip format, so it can always become a clip; its own
	 * change_data_type announces SBC at go-live, whatever audio_codec says
	 * (both apps decode by the announced type). */
	submit_gated(MG_AUDIO_LIVE, mg_data_type_audio_sbc, MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ, 0,
		     true, 0, true, MG_AUDIO_GATE_TURN);
	return 0;
}

int mg_audio_start_recording(uint8_t codec, uint16_t kbps)
{
	struct mg_audio_format fmt;

	if (!IS_ENABLED(CONFIG_MG_AUDIO_QUEUE) || mg_codec_format(codec, &fmt)) {
		return -ENOTSUP;
	}
	submit(MG_AUDIO_RECORDING, codec, MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ, kbps, false, 0);
	return 0;
}

int mg_audio_read_clip(uint16_t index)
{
	if (!IS_ENABLED(CONFIG_MG_AUDIO_QUEUE)) {
		return -ENOTSUP;
	}
	submit(MG_AUDIO_CLIP, 0, 0, 0, false, index);
	return 0;
}

int mg_audio_start_held(enum mg_audio_activity what, uint8_t codec, uint16_t rate, uint16_t kbps)
{
	struct mg_audio_format fmt;

	if (what == MG_AUDIO_RECORDING) {
		rate = MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ; /* clips stay 16 kHz */
	}
	if (mg_codec_format_cfg(codec, rate, kbps, &fmt) ||
	    (what != MG_AUDIO_LIVE && what != MG_AUDIO_RECORDING) ||
	    (what == MG_AUDIO_RECORDING && !IS_ENABLED(CONFIG_MG_AUDIO_QUEUE))) {
		return -ENOTSUP;
	}
#if defined(CONFIG_MG_SILENT_TURN_DROP)
	if (what == MG_AUDIO_LIVE) {
		/* A test/automation build's talk turn: its output (gestures
		 * included) waits until voice rises above room noise, and a turn
		 * that never does is dropped unsent (mg_voice.h). */
		int err;

		atomic_clear(&voice_seen);
		atomic_clear(&last_silent);
		err = out_arm(MG_AUDIO_GATE_VOICE);
		if (err == 0) {
			submit_gated(what, codec, rate, kbps, true, 0, true, MG_AUDIO_GATE_VOICE);
			return 0;
		}
		LOG_WRN("talk: no silent-turn check for this turn (%d)", err);
	}
#endif
	submit_held(what, codec, rate, kbps, what == MG_AUDIO_LIVE, 0, true);
	return 0;
}

void mg_audio_commit(void)
{
	atomic_cas(&commit_state, 0, 1);
}

void mg_audio_discard(void)
{
	atomic_cas(&commit_state, 0, 2);
	submit(MG_AUDIO_IDLE, 0, 0, 0, false, 0);
}

void mg_audio_stop(void)
{
	submit(MG_AUDIO_IDLE, 0, 0, 0, false, 0);
}

enum mg_audio_activity mg_audio_activity(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	enum mg_audio_activity a = req.want;

	k_mutex_unlock(&lock);
	return a;
}

void mg_audio_current_format(struct mg_audio_format *fmt)
{
	k_mutex_lock(&lock, K_FOREVER);
	*fmt = cur_fmt;
	k_mutex_unlock(&lock);
	if (fmt->type == mg_data_type_unknown) {
		(void)mg_codec_format(mg_settings_get()->audio_codec, fmt);
	}
}

void mg_audio_set_gain(uint8_t gain)
{
	/* set_mic_gain's level through the tuned gain (mg_mic_gain.h). */
	atomic_set(&gain_q10, (atomic_val_t)mg_mic_gain_q10(gain, CONFIG_MG_MIC_DEFAULT_GAIN,
							    MG_MIC_TUNED_Q10,
							    CONFIG_MG_MIC_GAIN_TOP_DB));
	atomic_set(&gain_level, gain);
}

static struct mg_hpf hpf;

/* Applies the gain; returns the samples that clipped (at the ADC's rail or
 * at the gain's clamp). */
static uint32_t apply_gain(int16_t *pcm, size_t n)
{
	int32_t g = (int32_t)atomic_get(&gain_q10);
	uint32_t clipped = 0;

	for (size_t i = 0; i < n; i++) {
		int32_t in = pcm[i];
		int32_t v = (in * g) >> 10;

		clipped += in >= INT16_MAX || in <= INT16_MIN + 1 || v > INT16_MAX ||
			   v < INT16_MIN;
		pcm[i] = (int16_t)CLAMP(v, INT16_MIN, INT16_MAX);
	}
	return clipped;
}

static void announce(const struct mg_audio_format *fmt)
{
	uint8_t buf[9];
	size_t n = mg_codec_change_data_type(fmt, buf);

	if (mg_audio_output_held()) {
		/* request_status shows it only once it's released. */
		k_mutex_lock(&out_lock, K_FOREVER);
		held_fmt = *fmt;
		k_mutex_unlock(&out_lock);
		(void)out_ctrl(buf, n);
		return;
	}
	k_mutex_lock(&lock, K_FOREVER);
	cur_fmt = *fmt;
	k_mutex_unlock(&lock);
	(void)mg_session_send_control(buf, n);
}

/* A live frame must fit the link's room, or every one would be dropped
 * after its format went out: the largest SBC bitpool or LC3 bitrate that
 * fits, before the format is announced (logged once per change). */
static void fit_live(struct mg_audio_format *fmt)
{
	static uint32_t logged;
	size_t room = MIN(mg_session_data_room(), sizeof(chunk));
	uint16_t want = fmt->frame_bytes;
	int rc = mg_codec_fit(fmt, room);
	uint32_t key = (uint32_t)fmt->type << 24 | (uint32_t)want << 12 | fmt->frame_bytes;

	if (rc == 0 || key == logged) {
		return;
	}
	logged = key;
	if (rc < 0) {
		LOG_ERR("live audio: a %u B frame doesn't fit the link's %u B", want,
			(unsigned int)room);
	} else if (fmt->type == mg_data_type_audio_sbc) {
		LOG_WRN("live audio: SBC frames cut from %u to %u B (bitpool %u) to fit the link's "
			"%u B", want, fmt->frame_bytes, fmt->sbc_bitpool, (unsigned int)room);
	} else {
		LOG_WRN("live audio: frames cut from %u to %u B (%u kb/s) to fit the link's %u B",
			want, fmt->frame_bytes,
			(unsigned int)(fmt->frame_bytes * 8000U / fmt->frame_us), (unsigned int)room);
	}
}

static size_t frames_per_chunk(const struct mg_audio_format *fmt)
{
	size_t room = MIN(mg_session_data_room(), sizeof(chunk));
	size_t by_time = MAX(1, CONFIG_MG_AUDIO_MAX_PACKET_MS * 1000 / fmt->frame_us);

	return MIN(room / fmt->frame_bytes, by_time);
}

/* The capture thread waits for the notification queue at most this long in
 * a row: the PDM driver's 8 blocks of 10 ms must not run out meanwhile. */
#define SEND_WAIT_MS 40

/* Live: whole frames packed into one Data notification each. */
struct packer {
	struct mg_audio_format fmt;
	size_t clen, nframes, fpc;
	uint32_t sent, dropped;
	bool full; /* the hold is full; the capture stops */
};

static void pack(struct packer *pk, const uint8_t *f, size_t n, k_timeout_t wait)
{
	if (pk->full) {
		return;
	}
	if (pk->nframes == 0) {
		pk->fpc = frames_per_chunk(&pk->fmt);
		if (pk->fpc == 0) {
			pk->dropped++;
			return; /* MTU too small for one frame */
		}
	}
	memcpy(&chunk[pk->clen], f, n);
	pk->clen += n;
	if (++pk->nframes == pk->fpc) {
		int err = out_data(chunk, pk->clen, wait);

		if (err == 0) {
			pk->sent++;
		} else if (err == -ENOSPC) {
			pk->full = true;
			LOG_WRN("mg.ble: the hold is full: capture stops, what it holds is kept");
		} else {
			pk->dropped++;
		}
		pk->clen = 0;
		pk->nframes = 0;
	}
}

#if defined(CONFIG_MG_AUDIO_QUEUE)
/* A clip's wall-clock start in unix seconds, back_ms before now; 0 when
 * the clock wasn't set (clip_info's start_unix_ms). */
static uint32_t clip_unix_s(uint32_t back_ms)
{
	int64_t ms;
	int16_t off;
	uint8_t flags;

	mg_clock_get(&ms, &off, &flags);
	if (ms == INT64_MIN || ms <= (int64_t)back_ms) {
		return 0;
	}
	return (uint32_t)((ms - back_ms) / 1000);
}
#endif

static bool commit_pending(const struct request *r)
{
	return (uint32_t)atomic_get(&commit_gen) == r->gen && atomic_get(&commit_state) == 1;
}

static void run_capture(const struct request *r)
{
	struct packer pk = {0};
	size_t nacc = 0;
	uint32_t stored = 0;
	bool live = r->want == MG_AUDIO_LIVE;
	bool gone = false; /* a gated capture's output discarded: nothing more goes */
#if defined(CONFIG_MG_AUDIO_QUEUE)
	bool full = false; /* the clip queue is full: frames stop going to it */
#endif
	bool qfail = false; /* the clip couldn't be opened at the commit */
	/* Pre-roll: nothing leaves until committed; a discard ends it silently. */
	bool committed = !r->held;
	size_t blen = 0, bhead = 0, bmax = 0;
	uint32_t bdropped = 0;

	if (mg_codec_format_cfg(r->codec, r->rate, r->kbps, &pk.fmt)) {
		LOG_ERR("codec %u at %u Hz unavailable", r->codec, r->rate);
		return;
	}
	if (live) {
		fit_live(&pk.fmt);
	}
	if (mg_codec_open_fmt(&pk.fmt)) {
		LOG_ERR("codec %u at %u Hz unavailable", r->codec, r->rate);
		return;
	}
	const struct mg_audio_format *fmt = &pk.fmt;

	if (r->held) {
		/* Whole frames, at most 1 s of them. */
		size_t per_s = (size_t)fmt->frame_bytes * (1000000u / fmt->frame_us);

		bmax = MIN(BACKLOG_BYTES, per_s) / fmt->frame_bytes * fmt->frame_bytes;
	}
	if (live && committed) {
		announce(fmt);
	}
#if defined(CONFIG_MG_AUDIO_QUEUE)
	if (!live && committed) {
		int err = mg_queue_begin_fmt(r->codec, fmt->sample_rate, fmt->channels,
					     fmt->frame_bytes, fmt->frame_us, k_uptime_get_32(),
					     clip_unix_s(0));

		if (err) {
			LOG_WRN("queue: can't record (%d)", err);
			mg_led_event(MG_LED_EVENT_NOT_KEPT);
			return;
		}
	}
#endif
	/* The mic indicator shows whenever the mic captures, pre-roll included,
	 * and is ON before the mic starts (visible
	 * before the first outbound frame): no light within 200 ms, no capture. */
	mg_led_set(live ? MG_LED_STREAMING : MG_LED_RECORDING, true);
	if (!mg_led_capture_wait(MG_AUDIO_LIGHT_WAIT_MS)) {
		atomic_inc(&light_refused);
		LOG_ERR("capture light not confirmed within %d ms: no capture", MG_AUDIO_LIGHT_WAIT_MS);
		goto out;
	}
	if (mg_audio_src_start(r->rate)) {
		LOG_ERR("audio source failed to start");
		goto out;
	}
	LOG_INF("%s capture, codec %u, %u Hz, %u B frames%s", live ? "live" : "offline", r->codec,
		r->rate, fmt->frame_bytes, committed ? "" : " (pre-roll until the hold threshold)");
	/* The first block primes the DC blocker: no settling step at the start. */
	mg_hpf_init(&hpf, CONFIG_MG_MIC_HPF_HZ, r->rate);
	mg_hpf_reset(&hpf);
	/* Encode time per frame, logged when the capture ends (the
	 * board check of 24 kHz LC3). */
	uint32_t enc_n = 0, enc_max = 0;
	uint64_t enc_sum = 0;
	int src_restarts = 0; /* in a row */
	/* This turn's capture level: what the encoder gets. */
	static struct mg_cap_meter meter;
	static struct mg_voice voice;

	mg_cap_begin(&meter, r->rate);
	mg_voice_init(&voice, r->rate);

	/* A release right after the threshold may stop the capture before this
	 * loop saw the commit: commit it all the same (button_down is out). */
	while ((current(r->gen) || (!committed && commit_pending(r))) && !pk.full) {
		mg_health_beat(MG_HEALTH_AUDIO);
		if (r->gate != MG_AUDIO_GATE_NONE && committed) {
			enum pump pr = out_pump();
			bool clipped = false;

#if defined(CONFIG_MG_AUDIO_QUEUE)
			if ((pr == PUMP_CLIP || pr == PUMP_CLIP_DROP) && live &&
			    held_to_clip(fmt, r->codec, &stored)) {
				/* The turn goes on into the clip. */
				clipped = true;
				live = false;
				mg_led_set(MG_LED_STREAMING, false);
				mg_led_set(MG_LED_RECORDING, true);
			}
#endif
			if (pr == PUMP_CLIP_DROP && !clipped) {
				out_discard("the link ended and no clip queue took the held turn",
					    &gstats.discards);
				pr = PUMP_DISCARD;
			}
			if (pr == PUMP_DISCARD) {
				gone = true;
				break;
			}
		}
		if (live && !mg_audio_output_held()) {
			/* Mic active (a held capture doesn't move the link). */
			mg_connparam_busy(MG_XFER_MIC);
		}
		if (!committed && commit_pending(r)) {
			/* Past the hold threshold: button_down is out. Now the
			 * format, then the backlog as a burst, then live. */
			committed = true;
			size_t start = (bhead + bmax - blen) % (bmax ? bmax : 1);

#if defined(CONFIG_MG_AUDIO_QUEUE)
			if (!live) {
				uint32_t back = blen / fmt->frame_bytes * fmt->frame_us / 1000;
				int err = mg_queue_begin_fmt(r->codec, fmt->sample_rate,
							     fmt->channels, fmt->frame_bytes,
							     fmt->frame_us, k_uptime_get_32() - back,
							     clip_unix_s(back));

				if (err) {
					LOG_WRN("queue: can't record (%d)", err);
					mg_led_event(MG_LED_EVENT_NOT_KEPT);
					full = true;
					qfail = true;
				}
			}
#endif
			if (live) {
				announce(fmt);
			}
			/* The burst waits for the queue no longer than the mic's
			 * blocks last in total. */
			int64_t until = k_uptime_get() + SEND_WAIT_MS;

			for (size_t i = 0; i < blen; i += fmt->frame_bytes) {
				const uint8_t *f = &backlog[(start + i) % bmax];

				if (live) {
					int64_t left = until - k_uptime_get();

					pack(&pk, f, fmt->frame_bytes, left > 0 ? K_MSEC(left) : K_NO_WAIT);
				}
#if defined(CONFIG_MG_AUDIO_QUEUE)
				else if (!full) {
					if (mg_queue_append(f, fmt->frame_bytes) == -ENOSPC) {
						LOG_WRN("queue full");
						full = true;
					} else {
						stored += fmt->frame_bytes;
					}
				}
#endif
			}
			LOG_INF("pre-roll: %u ms sent as a burst",
				(unsigned int)(blen / fmt->frame_bytes * fmt->frame_us / 1000));
			blen = 0;
		}
		int nb = mg_audio_src_read(block, 200);

		if (nb < 0) {
			/* The PDM driver stops when its blocks run out: start it
			 * again, or end the capture (and its light). */
			mg_audio_src_stop();
			if (++src_restarts > 3 || mg_audio_src_start(r->rate)) {
				LOG_ERR("mic read failed (%d): capture ends", nb);
				break;
			}
			LOG_WRN("mic read failed (%d): mic restarted", nb);
			continue;
		}
		if (nb == 0) {
			continue;
		}
		src_restarts = 0;
		mg_hpf_run(&hpf, block, nb);
		mg_cap_add(&meter, block, (uint32_t)nb, apply_gain(block, nb));
		if (r->gate == MG_AUDIO_GATE_VOICE && !atomic_get(&voice_seen) &&
		    mg_voice_add(&voice, block, (uint32_t)nb)) {
			atomic_set(&voice_seen, 1); /* the next pump releases the turn */
		}
		memcpy(&acc[nacc], block, nb * sizeof(block[0]));
		nacc += nb;

		while (nacc >= fmt->frame_samples) {
			uint32_t t0 = k_cycle_get_32();
			int n = mg_codec_encode(acc, frame, sizeof(frame));
			uint32_t dt = k_cyc_to_us_ceil32(k_cycle_get_32() - t0);

			enc_n++;
			enc_sum += dt;
			enc_max = MAX(enc_max, dt);

			nacc -= fmt->frame_samples;
			memmove(acc, &acc[fmt->frame_samples], nacc * sizeof(acc[0]));
			if (n <= 0) {
				continue;
			}
			if (!committed) {
				/* Backlog ring of whole frames; the oldest go first. */
				if (bmax == 0 || (size_t)n != fmt->frame_bytes) {
					bdropped++;
					continue;
				}
				memcpy(&backlog[bhead], frame, n);
				bhead = (bhead + n) % bmax;
				blen = MIN(blen + n, bmax);
				continue;
			}
			if (!live) {
#if defined(CONFIG_MG_AUDIO_QUEUE)
				if (!full && mg_queue_append(frame, n) == -ENOSPC) {
					LOG_WRN("queue full");
					full = true;
				}
				stored += full ? 0 : n;
#endif
				continue;
			}
			pack(&pk, frame, n, K_NO_WAIT);
		}
	}
	mg_audio_src_stop();
	if (enc_n) {
		LOG_INF("encode: %u frames at %u Hz, %u B each: %u us average, %u us most",
			enc_n, fmt->sample_rate, fmt->frame_bytes, (unsigned int)(enc_sum / enc_n),
			enc_max);
	}
	if (!committed) {
		/* A tap, or stopped before the threshold: nothing was sent. */
		LOG_INF("pre-roll discarded (%u ms): nothing sent",
			(unsigned int)(blen / fmt->frame_bytes * fmt->frame_us / 1000));
		ARG_UNUSED(bdropped);
		mg_led_set(live ? MG_LED_STREAMING : MG_LED_RECORDING, false);
		if (r->gate != MG_AUDIO_GATE_NONE) {
			uint32_t taps = 0;

			/* nothing of it was held */
			out_discard("a tap", r->gate == MG_AUDIO_GATE_VOICE ? &taps : &gstats.discards);
		}
		return;
	}
	if (live && pk.clen && !pk.full && !gone) {
		if (out_data(chunk, pk.clen, K_MSEC(100)) == 0) {
			pk.sent++;
		} else {
			pk.dropped++;
		}
	}
	LOG_INF("capture ended: %u chunks sent, %u dropped, %u bytes stored", pk.sent, pk.dropped,
		stored);
	{
		struct mg_cap_record rec;

		mg_cap_end(&meter, &rec, (uint8_t)atomic_get(&gain_level),
			   (uint32_t)atomic_get(&gain_q10), r->codec, live);
		cap_store(&rec);
		LOG_INF("capture level: peak %s%d.%d, rms %s%d.%d, floor %s%d.%d dBFS, "
			"%u clipped, gain level %u (%s%d.%d dB), %u ms",
			MG_DDB(rec.peak_ddb), MG_DDB(rec.rms_ddb), MG_DDB(rec.floor_ddb),
			rec.clipped, rec.gain_level, MG_DDB(rec.gain_ddb), rec.ms);
	}
out:
	if (live) {
		static const uint8_t stop[] = {mg_command_stop_mic};

		/* stop_mic ends every capture, whichever side started it. */
		mg_led_set(MG_LED_STREAMING, false);
		if (committed && !gone) {
			(void)out_ctrl(stop, sizeof(stop));
		}
		if (r->gate == MG_AUDIO_GATE_VOICE && !gone &&
		    mg_voice_turn_dropped(true, atomic_get(&voice_seen) != 0)) {
			/* Nothing above room noise: the turn, its gestures and
			 * stop_mic, never leave; the no-result cue (4 blinks) says
			 * "Didn't hear anything". */
			out_discard("talk: didn't hear anything (nothing above room noise)",
				    &gstats.silent);
			atomic_set(&last_silent, 1);
			mg_led_event(MG_LED_EVENT_NOT_KEPT);
			gone = true;
		}
		if (r->gate != MG_AUDIO_GATE_NONE && !gone) {
			/* What's held waits for the gate after the capture,
			 * 3 s at most from its end (a held turn: to the clip queue at
			 * its deadline); nothing new starts meanwhile. */
			k_mutex_lock(&out_lock, K_FOREVER);
			if (gate != MG_AUDIO_GATE_NONE) {
				cap_end = k_uptime_get_32() ?: 1;
			}
			k_mutex_unlock(&out_lock);
			while (mg_audio_output_held()) {
				mg_health_beat(MG_HEALTH_AUDIO);
				enum pump pr = out_pump();

				bool saved_clip = false;

#if defined(CONFIG_MG_AUDIO_QUEUE)
				if (pr == PUMP_CLIP || pr == PUMP_CLIP_DROP) {
					uint32_t saved = 0;

					if (held_to_clip(fmt, r->codec, &saved)) {
						bool kept = mg_queue_end() == 0 && saved;

						mg_led_event(kept ? MG_LED_EVENT_CLIP_KEPT
								  : MG_LED_EVENT_NOT_KEPT);
						saved_clip = true;
					}
				}
#endif
				if (pr == PUMP_CLIP_DROP && !saved_clip) {
					out_discard("the link ended and no clip queue took the held "
						    "turn",
						    &gstats.discards);
				}
				if (saved_clip || !mg_audio_output_held()) {
					break;
				}
				k_sleep(K_MSEC(20));
			}
		}
	} else {
		bool kept = stored > 0;

#if defined(CONFIG_MG_AUDIO_QUEUE)
		if (committed && !qfail && mg_queue_end()) {
			kept = false; /* a write failed: the clip wasn't saved */
		}
#endif
		mg_led_set(MG_LED_RECORDING, false);
		/* 2 blinks when the clip was kept, 4 when nothing was. */
		if (committed && !qfail) {
			mg_led_event(kept ? MG_LED_EVENT_CLIP_KEPT : MG_LED_EVENT_NOT_KEPT);
		}
	}
}

#if defined(CONFIG_MG_AUDIO_QUEUE)
static void run_clip(const struct request *r)
{
	struct mg_clip_info ci;
	struct mg_audio_format fmt = {.type = mg_data_type_audio_queue};
	uint32_t off = 0;

	if (mg_queue_clip_info(r->clip, &ci)) {
		return;
	}
	announce(&fmt);
	LOG_INF("sending clip %u (%u bytes)", r->clip, ci.bytes);
	int64_t t0 = k_uptime_get();
	uint32_t chunks = 0;

	while (off < ci.bytes && current(r->gen)) {
		mg_health_beat(MG_HEALTH_AUDIO);
		mg_connparam_busy(MG_XFER_CLIP);
		size_t n = MIN(MIN(mg_session_data_room(), sizeof(chunk)), ci.bytes - off);

		if (n == 0 || mg_queue_read(r->clip, off, chunk, n)) {
			break;
		}
		if (!mg_core_output_ok()) {
			LOG_INF("clip download stopped: mic output no longer allowed");
			break;
		}
		int err = mg_session_send_data(chunk, n, K_MSEC(200));

		if (err == -ENOMEM) {
			continue; /* congested: retry the same chunk */
		}
		if (err) {
			LOG_WRN("clip download stopped (%d)", err);
			break;
		}
		off += n;
		chunks++;
	}
	uint32_t ms = MAX((uint32_t)(k_uptime_get() - t0), 1U);

	/* Queued, not on the air yet: the notification queue holds most of a clip, so
	 * the bench client times the download itself. */
	LOG_INF("clip %u: %u of %u bytes queued in %u ms (%u notifications)", r->clip, off,
		ci.bytes, ms, chunks);
}
#endif

static void audio_thread(void *a, void *b, void *c)
{
	struct request r;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		mg_health_busy(MG_HEALTH_AUDIO, false);
		k_sem_take(&wake, K_FOREVER);
		mg_health_busy(MG_HEALTH_AUDIO, true);
		for (;;) {
			k_mutex_lock(&lock, K_FOREVER);
			r = req;
			k_mutex_unlock(&lock);
			if (r.gate == MG_AUDIO_GATE_NONE) {
				/* A gated capture superseded before this thread read
				 * it never ran, so nothing would disarm its hold:
				 * dropped here, or later output would go into it and
				 * gated starts get busy until a reboot. A gated capture
				 * that ran and ended (cap_end set) keeps its hold for
				 * the proof. */
				k_mutex_lock(&out_lock, K_FOREVER);
				bool unstarted = gate != MG_AUDIO_GATE_NONE && cap_end == 0 &&
						 hold_gen != 0 && (int32_t)(r.gen - hold_gen) > 0;

				k_mutex_unlock(&out_lock);
				if (unstarted) {
					out_discard("superseded before it started", &gstats.discards);
				}
			}

			switch (r.want) {
			case MG_AUDIO_LIVE:
			case MG_AUDIO_RECORDING:
				run_capture(&r);
				break;
#if defined(CONFIG_MG_AUDIO_QUEUE)
			case MG_AUDIO_CLIP:
				run_clip(&r);
				break;
#endif
			default:
				break;
			}
			/* Finished on its own, or stopped: go idle unless something
			 * new arrived meanwhile. */
			k_mutex_lock(&lock, K_FOREVER);
			bool again = req.gen != r.gen && req.want != MG_AUDIO_IDLE;

			if (!again) {
				req.want = MG_AUDIO_IDLE;
			}
			k_mutex_unlock(&lock);
			if (!again) {
				break;
			}
		}
		k_work_submit_to_queue(mg_app_wq(), &idle_work);
	}
}

K_THREAD_STACK_DEFINE(audio_stack, STACK_SIZE);
static struct k_thread audio_tid;

__weak int mg_audio_src_probe(void)
{
	return 0;
}

void mg_audio_init(void)
{
	k_work_init(&idle_work, idle_fn);
	mg_hpf_init(&hpf, CONFIG_MG_MIC_HPF_HZ, 16000);
	k_thread_create(&audio_tid, audio_stack, K_THREAD_STACK_SIZEOF(audio_stack), audio_thread,
			NULL, NULL, NULL, K_PRIO_PREEMPT(4), 0, K_NO_WAIT);
	k_thread_name_set(&audio_tid, "mg_audio");
}
